#include <array>
#include <catch2/catch_test_macros.hpp>
#include <ranges>
#include <snapfit/snapfit.h>
#include <tuple>
#include <type_traits>
#include <utility>

namespace
{
    using entity = snapfit::entity32;
    using traits = snapfit::traits<entity>;
    using registry = snapfit::registry<traits>;

    [[nodiscard]] constexpr entity make_entity(traits::index_type index,
                                               traits::generation_type generation = 0)
    {
        return snapfit::details::make_entity<traits>(index, generation);
    }

    [[nodiscard]] constexpr traits::index_type index_of(entity value)
    {
        return snapfit::details::entity_index<traits>(value);
    }

    struct position {
        int x;
    };

    struct velocity {
        int x;
    };

    struct disabled_label;
    struct selected_label;
    using disabled = snapfit::tag<disabled_label>;
    using selected = snapfit::tag<selected_label>;

    template<typename Registry, typename Component>
    concept gettable = requires(Registry& value, typename Registry::entity ent) {
        value.template get<Component>(ent);
    };

    template<typename Registry, typename Component>
    concept try_gettable = requires(Registry& value, typename Registry::entity ent) {
        value.template try_get<Component>(ent);
    };

    enum class tiny_entity : std::uint32_t
    {
    };

    template<typename Policy>
    struct tiny_entity_traits {
        using entity = tiny_entity;
        using underlying_type = std::underlying_type_t<entity>;
        using generation_type = std::uint32_t;
        using index_type = std::uint32_t;
        using wrap_policy = Policy;

        static constexpr std::size_t index_bits = 2;
        static constexpr std::size_t generation_bits = 2;
        static constexpr underlying_type index_mask = snapfit::bitmask<underlying_type>(index_bits);
        static constexpr underlying_type generation_mask =
            snapfit::bitmask<underlying_type>(generation_bits) << index_bits;
        static constexpr std::size_t generation_shift = index_bits;

        static constexpr entity null { std::numeric_limits<underlying_type>::max() };
        static constexpr index_type null_index = 3;
        static constexpr index_type max_index = 2;
        static constexpr generation_type tombstone = 3;
        static constexpr generation_type max_generation = 2;
    };

    template<typename TinyTraits>
    [[nodiscard]] constexpr auto tiny_index(tiny_entity value)
    {
        return snapfit::details::entity_index<TinyTraits>(value);
    }

    template<typename TinyTraits>
    [[nodiscard]] constexpr auto tiny_generation(tiny_entity value)
    {
        return snapfit::details::entity_generation<TinyTraits>(value);
    }
} // namespace

TEST_CASE("request expands the registry and frees skipped indices")
{
    registry value;
    const auto hint = make_entity(5, 7);

    REQUIRE(value.request(hint) == hint);

    for (traits::index_type expected = 0; expected < 5; ++expected)
    {
        CHECK(index_of(value.create()) == expected);
    }
}

TEST_CASE("create fills a mutable iterator range")
{
    registry value;
    std::array<entity, 3> created {};

    value.create(created.begin(), created.end());

    CHECK(index_of(created[0]) == 0);
    CHECK(index_of(created[1]) == 1);
    CHECK(index_of(created[2]) == 2);
}

TEST_CASE("create count returns the requested entities")
{
    registry value;

    const auto created = value.create(3);

    REQUIRE(created.size() == 3);
    CHECK(index_of(created[0]) == 0);
    CHECK(index_of(created[1]) == 1);
    CHECK(index_of(created[2]) == 2);
}

TEST_CASE("bulk create reuses free slots before growing")
{
    registry value;

    const auto zero = value.create();
    const auto one = value.create();
    value.release(zero);

    std::array<entity, 3> created {};
    value.create(created.begin(), created.end());

    CHECK(index_of(created[0]) == index_of(zero));
    CHECK(index_of(created[1]) == 2);
    CHECK(index_of(created[2]) == 3);
    CHECK(one != created[0]);
}

TEST_CASE("request removes the free-list head")
{
    registry value;

    REQUIRE(value.request(make_entity(5, 7)).has_value());
    REQUIRE(value.request(make_entity(0, 8)).has_value());

    CHECK(index_of(value.create()) == 1);
}

TEST_CASE("request unlinks a middle free node without losing earlier nodes")
{
    registry value;

    REQUIRE(value.request(make_entity(5, 7)).has_value());
    REQUIRE(value.request(make_entity(2, 8)).has_value());

    std::array<traits::index_type, 4> actual {};
    for (auto& index : actual)
    {
        index = index_of(value.create());
    }

    CHECK(actual == std::array<traits::index_type, 4> { 0, 1, 3, 4 });
}

TEST_CASE("request rejects an occupied index regardless of generation")
{
    registry value;
    const auto active = value.create();
    const auto occupied_index = index_of(active);

    CHECK_FALSE(value.request(active).has_value());
    CHECK_FALSE(value.request(make_entity(occupied_index, 42)).has_value());
}

TEST_CASE("release invalidates stale handles and increments the generation")
{
    registry value;
    const auto stale = value.create();

    value.emplace<position>(stale, 10);
    value.release(stale);

    const auto replacement = value.create();
    CHECK(index_of(replacement) == index_of(stale));
    CHECK(replacement != stale);
    CHECK_FALSE(value.contains<position>(stale));
    CHECK_THROWS_AS(value.emplace<position>(stale, 20), snapfit::invalid_entity);
}

TEST_CASE("component operations follow entity lifetime")
{
    registry value;
    const auto ent = value.create();

    auto& component = value.emplace<position>(ent, 11);
    CHECK(component.x == 11);
    CHECK(value.contains<position>(ent));
    CHECK(value.get<position>(ent).x == 11);

    CHECK(value.remove<position>(ent));
    CHECK_FALSE(value.contains<position>(ent));
    CHECK_FALSE(value.remove<position>(ent));
    CHECK_THROWS_AS(value.get<position>(ent), snapfit::no_such_component);
}

TEST_CASE("removing a non-last component preserves the remaining dense elements")
{
    registry value;
    const auto first = value.create();
    const auto middle = value.create();
    const auto last = value.create();
    value.emplace<position>(first, 1);
    value.emplace<position>(middle, 2);
    value.emplace<position>(last, 3);

    REQUIRE(value.remove<position>(middle));

    CHECK(value.get<position>(first).x == 1);
    CHECK(value.get<position>(last).x == 3);
    CHECK_FALSE(value.contains<position>(middle));
}

TEST_CASE("get supports zero, one, and multiple component types")
{
    registry value;
    const auto ent = value.create();
    value.emplace<position>(ent, 11);
    value.emplace<velocity>(ent, 7);

    static_assert(std::is_same_v<decltype(value.get<>(ent)), std::tuple<>>);
    static_assert(std::is_same_v<decltype(value.get<position>(ent)), position&>);
    static_assert(std::is_same_v<decltype(value.get<position, velocity>(ent)),
                                 std::tuple<position&, velocity&>>);

    auto [pos, vel] = value.get<position, velocity>(ent);
    pos.x = 12;
    vel.x = 8;

    CHECK(value.get<position>(ent).x == 12);
    CHECK(value.get<velocity>(ent).x == 8);

    const registry& const_value = value;
    static_assert(std::is_same_v<decltype(const_value.get<position>(ent)), const position&>);
    static_assert(std::is_same_v<decltype(const_value.get<position, velocity>(ent)),
                                 std::tuple<const position&, const velocity&>>);
}

TEST_CASE("empty get still validates the entity")
{
    registry value;

    CHECK_THROWS_AS(value.get<>(make_entity(0)), snapfit::invalid_entity);
}

TEST_CASE("wrap policy reuses an exhausted index at generation zero")
{
    using tiny_traits = tiny_entity_traits<snapfit::wrap_generation>;
    snapfit::registry<tiny_traits> value;

    auto ent = value.create();
    REQUIRE(tiny_generation<tiny_traits>(ent) == 0);

    value.release(ent);
    ent = value.create();
    REQUIRE(tiny_generation<tiny_traits>(ent) == 1);

    value.release(ent);
    ent = value.create();
    REQUIRE(tiny_generation<tiny_traits>(ent) == tiny_traits::max_generation);

    value.release(ent);
    const auto wrapped = value.create();

    CHECK(tiny_index<tiny_traits>(wrapped) == 0);
    CHECK(tiny_generation<tiny_traits>(wrapped) == 0);
}

TEST_CASE("tombstone policy permanently retires an exhausted index")
{
    using tiny_traits = tiny_entity_traits<snapfit::tombstone_on_wrap>;
    snapfit::registry<tiny_traits> value;

    auto ent = value.create();
    value.release(ent);
    ent = value.create();
    value.release(ent);
    ent = value.create();
    REQUIRE(tiny_generation<tiny_traits>(ent) == tiny_traits::max_generation);

    value.release(ent);
    const auto next = value.create();

    CHECK(tiny_index<tiny_traits>(next) == 1);

    std::array<tiny_entity, 2> too_many {};
    CHECK_THROWS_AS(value.create(too_many.begin(), too_many.end()), std::length_error);
}

TEST_CASE("view visits entities that own every requested component")
{
    registry value;

    const auto matching = value.create();
    value.emplace<position>(matching, 1);
    value.emplace<velocity>(matching, 2);

    const auto position_only = value.create();
    value.emplace<position>(position_only, 3);

    std::size_t visits = 0;
    value.view<position, velocity>(
        [&](const entity& ent, position& pos, velocity& vel)
        {
            CHECK(ent == matching);
            pos.x += vel.x;
            ++visits;
        });

    CHECK(visits == 1);
    CHECK(value.get<position>(matching).x == 3);

    const registry& const_value = value;
    const_value.view<position, velocity>([](const entity&, const position&, const velocity&) {});
}

TEST_CASE("empty view visits every live entity")
{
    registry value;
    (void)value.create();
    const auto released = value.create();
    (void)value.create();
    value.release(released);

    std::size_t visits = 0;
    value.view<>([&](const entity&) { ++visits; });

    CHECK(visits == 2);
}

TEST_CASE("view with a missing required storage is empty")
{
    registry value;
    const auto ent = value.create();
    value.emplace<position>(ent, 1);

    std::size_t visits = 0;
    value.view<position, velocity>([&](const entity&, position&, velocity&) { ++visits; });

    CHECK(visits == 0);
}

TEST_CASE("tags provide presence without a retrievable component")
{
    static_assert(!gettable<registry, disabled>);
    static_assert(!gettable<const registry, disabled>);
    static_assert(!try_gettable<registry, disabled>);
    static_assert(!try_gettable<const registry, disabled>);

    registry value;
    const auto first = value.create();
    const auto middle = value.create();
    const auto last = value.create();

    static_assert(std::is_void_v<decltype(value.emplace<disabled>(first))>);
    value.emplace<disabled>(first);
    value.emplace<disabled>(middle);
    value.emplace<disabled>(last);

    CHECK(value.contains<disabled>(first));
    CHECK_THROWS_AS(value.emplace<disabled>(first), snapfit::duplicate_component);

    CHECK(value.remove<disabled>(middle));
    CHECK_FALSE(value.contains<disabled>(middle));
    CHECK(value.contains<disabled>(first));
    CHECK(value.contains<disabled>(last));

    value.release(first);
    CHECK_FALSE(value.contains<disabled>(first));
}

TEST_CASE("tag exclusions filter views without becoming callback arguments")
{
    registry value;
    const auto enabled = value.create();
    const auto disabled_entity = value.create();
    value.emplace<position>(enabled, 1);
    value.emplace<position>(disabled_entity, 2);
    value.emplace<disabled>(disabled_entity);
    value.emplace<selected>(enabled);

    std::size_t visits = 0;
    value.view<position>(snapfit::exclude<disabled>,
                         [&](const entity& ent, position& pos)
                         {
                             CHECK(ent == enabled);
                             ++pos.x;
                             ++visits;
                         });

    CHECK(visits == 1);
    CHECK(value.get<position>(enabled).x == 2);
    CHECK(value.get<position>(disabled_entity).x == 2);

    const registry& const_value = value;
    const_value.view<position>(snapfit::exclude<disabled>, [](const entity&, const position&) {});
}

TEST_CASE("tag inclusions and exclusions jointly filter views")
{
    registry value;

    const auto matching = value.create();
    value.emplace<position>(matching, 1);
    value.emplace<selected>(matching);

    const auto missing_include = value.create();
    value.emplace<position>(missing_include, 2);

    const auto excluded = value.create();
    value.emplace<position>(excluded, 3);
    value.emplace<selected>(excluded);
    value.emplace<disabled>(excluded);

    const auto missing_component = value.create();
    value.emplace<selected>(missing_component);

    std::size_t visits = 0;
    value.view<position>(snapfit::include<selected>,
                         snapfit::exclude<disabled>,
                         [&](const entity& ent, position& pos)
                         {
                             CHECK(ent == matching);
                             ++pos.x;
                             ++visits;
                         });

    CHECK(visits == 1);
    CHECK(value.get<position>(matching).x == 2);
    CHECK(value.get<position>(missing_include).x == 2);
    CHECK(value.get<position>(excluded).x == 3);

    const registry& const_value = value;
    std::size_t const_visits = 0;
    const_value.view<position>(snapfit::include<selected>,
                               snapfit::exclude<disabled>,
                               [&](const entity& ent, const position& pos)
                               {
                                   CHECK(ent == matching);
                                   CHECK(pos.x == 2);
                                   ++const_visits;
                               });

    CHECK(const_visits == 1);
}

TEST_CASE("tag-only view uses included tags as iteration candidates")
{
    registry value;
    const auto matching = value.create();
    const auto excluded = value.create();
    (void)value.create();
    value.emplace<selected>(matching);
    value.emplace<selected>(excluded);
    value.emplace<disabled>(excluded);

    std::size_t visits = 0;
    value.view<>(snapfit::include<selected>,
                 snapfit::exclude<disabled>,
                 [&](const entity& ent)
                 {
                     CHECK(ent == matching);
                     ++visits;
                 });
    CHECK(visits == 1);

    const registry& const_value = value;
    std::size_t const_visits = 0;
    const_value.view<>(snapfit::include<selected>,
                       snapfit::exclude<disabled>,
                       [&](const entity&) { ++const_visits; });
    CHECK(const_visits == 1);
}

TEST_CASE("view is empty when the first requested component storage is missing")
{
    registry value;
    auto entities = value.create(5);
    value.emplace<velocity>(entities[0], 5);

    std::size_t visits = 0;
    value.view<position, velocity>(
        [&](const entity&, position&, velocity&) { ++visits; });

    CHECK(visits == 0);
}

TEST_CASE("try_get returns pointers for present components and null otherwise")
{
    registry value;
    const auto ent = value.create();

    {
        auto* p = value.try_get<position>(ent);
        CHECK(p == nullptr);
    }

    {
        auto& c = value.emplace<position>(ent, 42);
        auto* p = value.try_get<position>(ent);
        CHECK(p == std::addressof(c));
        (void)value.remove<position>(ent);
    }

    {
        const auto other = value.create();
        value.emplace<position>(other, 9);
        CHECK(value.try_get<position>(ent) == nullptr);
    }

    {
        auto& c = value.emplace<position>(ent, 1);
        auto t = value.try_get<position, velocity>(ent);
        CHECK(std::get<0>(t) == std::addressof(c));
        CHECK(std::get<1>(t) == nullptr);
        (void)value.remove<position>(ent);
    }
}

TEST_CASE("range view models an input range")
{
    using view_type =
        decltype(std::declval<registry&>().view<position>(snapfit::include<>, snapfit::exclude<>));

    using iterator = std::ranges::iterator_t<view_type>;

    static_assert(std::default_initializable<iterator>);
    static_assert(std::movable<iterator>);
    static_assert(std::weakly_incrementable<iterator>);
    static_assert(std::indirectly_readable<iterator>);
    static_assert(std::input_iterator<iterator>);
    static_assert(std::ranges::input_range<view_type>);
}

TEST_CASE("range view yields requested components by mutable reference")
{
    registry value;

    const auto matching = value.create();
    value.emplace<position>(matching, 1);
    value.emplace<velocity>(matching, 2);

    const auto missing_velocity = value.create();
    value.emplace<position>(missing_velocity, 10);

    auto matching_view = value.view<position, velocity>();
    using reference = std::ranges::range_reference_t<decltype(matching_view)>;
    static_assert(std::same_as<reference, std::tuple<entity, position&, velocity&>>);

    std::size_t visits = 0;
    for (auto [ent, pos, vel] : matching_view)
    {
        CHECK(ent == matching);
        pos.x += vel.x;
        ++visits;
    }

    CHECK(visits == 1);
    CHECK(value.get<position>(matching).x == 3);
    CHECK(value.get<position>(missing_velocity).x == 10);
}

TEST_CASE("range view applies included and excluded filters")
{
    registry value;

    const auto matching = value.create();
    value.emplace<position>(matching, 1);
    value.emplace<selected>(matching);

    const auto missing_include = value.create();
    value.emplace<position>(missing_include, 2);

    const auto excluded = value.create();
    value.emplace<position>(excluded, 3);
    value.emplace<selected>(excluded);
    value.emplace<disabled>(excluded);

    std::size_t visits = 0;
    for (auto [ent, pos] :
         value.view<position>(snapfit::include<selected>, snapfit::exclude<disabled>))
    {
        CHECK(ent == matching);
        CHECK(pos.x == 1);
        ++visits;
    }

    CHECK(visits == 1);
}

TEST_CASE("const range view yields components by const reference")
{
    registry value;
    const auto ent = value.create();
    value.emplace<position>(ent, 42);

    const registry& const_value = value;
    auto positions = const_value.view<position>();
    using reference = std::ranges::range_reference_t<decltype(positions)>;
    static_assert(std::same_as<reference, std::tuple<entity, const position&>>);

    std::size_t visits = 0;
    for (auto [found, pos] : positions)
    {
        CHECK(found == ent);
        CHECK(pos.x == 42);
        ++visits;
    }

    CHECK(visits == 1);
}

TEST_CASE("empty range view yields every live entity")
{
    registry value;
    const auto first = value.create();
    const auto released = value.create();
    const auto last = value.create();
    value.release(released);

    std::size_t visits = 0;
    for (auto [ent] : value.view<>())
    {
        CHECK((ent == first || ent == last));
        ++visits;
    }

    CHECK(visits == 2);
}

TEST_CASE("range view is empty when a required storage is missing")
{
    registry value;
    const auto ent = value.create();
    value.emplace<position>(ent, 1);

    const auto missing_storage_view = value.view<position, velocity>();

    CHECK(missing_storage_view.begin() == missing_storage_view.end());
}
