#include <cinttypes>
#include <snapfit/snapfit.h>
#include <print>

namespace
{
    struct position {
        int x;
    };

    struct tag;
} // namespace

template<typename... Types>
struct typelist {
    template<typename... Ts>
    struct unique_types;

    template<typename T>
    struct unique_types<T> {
        static constexpr bool value = true;
    };

    template<typename Head, typename... Tail>
    struct unique_types<Head, Tail...> {
        static constexpr bool value =
            !typelist<Tail...>::template includes<Head>() && unique_types<Tail...>::value;
    };

    static consteval bool unique()
    {
        return unique_types<Types...>::value;
    }

    template<typename T>
    static consteval bool includes()
    {
        return std::disjunction_v<std::is_same<T, Types>...>;
    }

    template<std::size_t Index, typename Head, typename... Tail>
    struct get_type_impl;

    template<typename Head, typename... Tail>
    struct get_type_impl<0, Head, Tail...> {
        using type = Head;
    };

    template<std::size_t Index, typename Head, typename... Tail>
    struct get_type_impl {
        using type = typename get_type_impl<Index - 1, Tail...>::type;
    };

    template<std::size_t Index>
    struct get_type_at {
        using type = typename get_type_impl<Index, Types...>::type;
    };

    template<std::size_t Index>
    using get = typename get_type_at<Index>::type;

    template<typename Type, std::size_t Index, typename Head, typename... Tail>
    struct index_of;

    template<typename Type, size_t Index, typename... Tail>
    struct index_of<Type, Index, Type, Tail...> {
        static constexpr std::size_t value = Index;
    };

    template<typename Type, std::size_t Index, typename Head, typename... Tail>
    struct index_of {
        static constexpr std::size_t value = index_of<Type, Index + 1, Tail...>::value;
    };

    template<typename Type>
    struct index_of_type {
        static constexpr std::size_t value = index_of<Type, 0, Types...>::value;
    };

    template<typename Type>
    static constexpr auto index = index_of_type<Type>::value;
};

template<typename... Types>
class variant
{
    using types = typelist<Types...>;

    static_assert(types::unique());

public:
    constexpr variant() noexcept(std::is_nothrow_constructible_v<typename types::template get<0>>)
        requires std::default_initializable<typename types::template get<0>>
    {
        using first_type = typename types::template get<0>;
        std::construct_at(reinterpret_cast<first_type*>(std::addressof(buffer)));
    }

    template<typename T, typename... Args>
        requires(types::template includes<T>() && std::is_constructible_v<T, Args && ...>)
    constexpr explicit variant(std::in_place_type_t<T>, Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...>)
    {
        std::construct_at<T>(reinterpret_cast<T*>(std::addressof(buffer)),
                             std::forward<Args>(args)...);
        index = types::template index<T>;
    }

    ~variant() noexcept
    {
        destroy_impl<0>();
    }

    template<typename T>
        requires(types::template includes<T>())
    constexpr T& get()
    {
        const auto type_index = types::template index<T>;
        if (type_index != index)
        {
            throw std::bad_variant_access {};
        }

        return *std::launder(reinterpret_cast<T*>(std::addressof(buffer)));
    }

    template<typename Visitor>
        requires(std::invocable<Visitor &&, Types&> && ...)
    constexpr decltype(auto) visit(Visitor&& visitor)
    {
        return visit_impl<0>(std::forward<Visitor>(visitor));
    }

private:
    std::size_t index {};
    alignas(Types...) std::byte buffer[std::max({ sizeof(Types)... })];

    template<std::size_t Index, typename Visitor>
    constexpr decltype(auto) visit_impl(Visitor&& visitor)
    {
        if constexpr (Index == sizeof...(Types))
        {
            throw std::bad_variant_access {};
        }
        else
        {
            using current_type = typename types::template get<Index>;
            if (index == Index)
            {
                return std::invoke(std::forward<Visitor>(visitor), get<current_type>());
            }
            else
            {
                return visit_impl<Index + 1>(std::forward<Visitor>(visitor));
            }
        }
    }

    template<std::size_t Index>
    constexpr void destroy_impl() noexcept
    {
        if constexpr (Index == sizeof...(Types))
        {
            // wth
        }
        else
        {
            using current_type = typename types::template get<Index>;
            if (index == Index)
            {
                std::destroy_at(
                    std::launder(reinterpret_cast<current_type*>(std::addressof(buffer))));
            }
            else
            {
                destroy_impl<Index + 1>();
            }
        }
    }
};

template<>
class variant<>
{ };

class C
{
public:
    ~C()
    {
        std::print("C::~C\n");
    }
};

int main()
{
    using traits32 = snapfit::traits<snapfit::entity32>;
    using registry32 = snapfit::registry<traits32>;

    registry32 registry;

    const auto ent = registry.create();

    registry.emplace<position>(ent, 5);
    std::print("position.x = {}\n", registry.get<position>(ent).x);

    registry.emplace<snapfit::tag<tag>>(ent);

    registry.release(ent);
    std::print("released = {}\n", !registry.contains<position>(ent));

    [[maybe_unused]] variant<> v0;
    variant<std::size_t> v1;
    variant<uint16_t, uint32_t, std::string> v2 { std::in_place_type<uint32_t>, 42 };

    v2.visit([](auto&& arg) { std::print("{}\n", arg); });

    std::print("{}\n", v2.get<uint32_t>());

    {
        variant<C> v;
    }

    return 0;
}
