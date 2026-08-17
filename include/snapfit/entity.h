#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace snapfit
{
    struct wrap_generation { };
    struct tombstone_on_wrap { };
    struct assert_on_wrap { };

    template<typename Type, typename... Types>
    inline constexpr bool is_one_of_v = (std::is_same_v<Type, Types> || ...);

    /// A packed entity handle type represented by an unsigned scoped enumeration.
    template<typename Type>
    concept entity_type = std::is_enum_v<Type> && std::is_unsigned_v<std::underlying_type_t<Type>>;

    /// Describes the index, generation, and sentinel layout of an entity type.
    ///
    /// A conforming traits type must reserve an index above `max_index` for
    /// `null_index`; consequently, usable indices form the inclusive range
    /// `[0, max_index]`.
    template<typename Type>
    concept traits_type = requires {
        typename Type::entity;
        typename Type::underlying_type;
        typename Type::generation_type;
        typename Type::index_type;
        typename Type::wrap_policy;
        typename std::integral_constant<decltype(Type::index_bits), Type::index_bits>;
        typename std::integral_constant<decltype(Type::generation_bits), Type::generation_bits>;
        typename std::integral_constant<decltype(Type::index_mask), Type::index_mask>;
        typename std::integral_constant<decltype(Type::generation_mask), Type::generation_mask>;
        typename std::integral_constant<decltype(Type::generation_shift), Type::generation_shift>;
        typename std::integral_constant<typename Type::entity, Type::null>;
        typename std::integral_constant<typename Type::index_type, Type::null_index>;
        typename std::integral_constant<typename Type::index_type, Type::max_index>;
        typename std::integral_constant<typename Type::generation_type, Type::max_generation>;
        typename std::integral_constant<typename Type::generation_type, Type::tombstone>;
        typename std::integral_constant<std::size_t, Type::component_cache_size>;
        requires(Type::max_index < Type::null_index);
        requires(Type::max_generation < Type::tombstone);
        requires(is_one_of_v<typename Type::wrap_policy,
                             wrap_generation,
                             tombstone_on_wrap,
                             assert_on_wrap>);
    };

    /// A multipass iterator that reads and replaces entity handles.
    template<typename Iterator, typename Entity>
    concept mutable_entity_iterator =
        std::forward_iterator<Iterator>
        && std::same_as<std::remove_cv_t<std::iter_value_t<Iterator>>, Entity>
        && std::indirectly_writable<Iterator, Entity>;

    /// Defines the packed representation of an entity handle.
    ///
    /// Specialize this template to use a custom entity type with `registry`.
    template<entity_type Entity, typename WrapPolicy = wrap_generation>
    struct traits;

    template<std::integral Type>
    constexpr Type bitmask(auto bits) noexcept
    {
        return static_cast<Type>((1 << bits) - 1);
    }

    /// A 32-bit entity handle with a 20-bit index and 12-bit generation.
    enum class entity32 : std::uint32_t
    {
    };

    /// Packed-layout traits for `entity32`.
    template<typename WrapPolicy>
    struct traits<entity32, WrapPolicy> {
        static_assert(std::is_same_v<std::underlying_type_t<entity32>, std::uint32_t>);

        using entity = entity32;
        using underlying_type = std::underlying_type_t<entity32>;
        using generation_type = std::uint16_t;
        using index_type = std::uint32_t;
        using wrap_policy = WrapPolicy;

        static constexpr std::size_t index_bits = 20;
        static constexpr std::size_t generation_bits = 12;

        static constexpr auto index_mask = bitmask<underlying_type>(index_bits);
        static constexpr auto generation_mask = bitmask<underlying_type>(generation_bits)
                                                << index_bits;
        static constexpr auto generation_shift = index_bits;

        static constexpr entity null { std::numeric_limits<std::underlying_type_t<entity>>::max() };
        static constexpr index_type null_index = std::to_underlying(null) & index_mask;
        static constexpr index_type max_index = bitmask<index_type>(index_bits) - 1;
        static constexpr generation_type tombstone = bitmask<generation_type>(generation_bits) - 1;
        static constexpr generation_type max_generation =
            bitmask<generation_type>(generation_bits) - 2;
        static constexpr std::size_t component_cache_size = 2;
    };

    /// Thrown when an entity does not own a requested component.
    class no_such_component : public std::runtime_error
    {
    public:
        no_such_component()
            : std::runtime_error { "no such component" }
        {
        }
    };

    /// Thrown when an entity already owns a component of the requested type.
    class duplicate_component : public std::runtime_error
    {
    public:
        duplicate_component()
            : std::runtime_error { "component already exists" }
        {
        }
    };

    /// Thrown when an operation requires a live entity but receives a stale or invalid handle.
    class invalid_entity : public std::runtime_error
    {
    public:
        invalid_entity()
            : std::runtime_error { "invalid entity" }
        {
        }
    };

    namespace details
    {
        using type_id = std::size_t;

        [[nodiscard]] inline type_id next_type_id() noexcept
        {
            static type_id id { 0 };
            return id++;
        }

        template<typename T>
        [[nodiscard]] type_id id_of() noexcept
        {
            static type_id id { next_type_id() };
            return id;
        }

        template<traits_type Traits>
        [[nodiscard]]
        constexpr auto
        entity_index(const typename Traits::entity ent) noexcept -> Traits::index_type
        {
            using index_type = typename Traits::index_type;

            return static_cast<index_type>(std::to_underlying(ent) & Traits::index_mask);
        }

        template<traits_type Traits>
        [[nodiscard]]
        constexpr auto
        entity_generation(const typename Traits::entity ent) noexcept -> Traits::generation_type
        {
            using generation_type = typename Traits::generation_type;

            return static_cast<generation_type>((std::to_underlying(ent) & Traits::generation_mask)
                                                >> Traits::generation_shift);
        }

        template<traits_type Traits>
        [[nodiscard]]
        constexpr auto
        make_entity(const typename Traits::index_type index,
                    const typename Traits::generation_type generation) noexcept -> Traits::entity
        {
            using underlying_type = typename Traits::underlying_type;
            using entity = typename Traits::entity;
            return entity { (static_cast<underlying_type>(generation) << Traits::generation_shift)
                            | (index & Traits::index_mask) };
        }
    } // namespace details

} // namespace snapfit
