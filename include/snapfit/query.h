#pragma once

#include <type_traits>

namespace snapfit
{
    template<typename... Components>
    struct include_t { };

    template<typename... Components>
    inline constexpr include_t<Components...> include {};

    template<typename... Components>
    struct exclude_t { };

    template<typename... Components>
    inline constexpr exclude_t<Components...> exclude {};

    template<typename... Types>
    struct type_list_t {
        template<typename... Ts>
        struct unique_types;

        template<typename T>
        struct unique_types<T> {
            static constexpr bool value = true;
        };

        template<typename Head, typename... Tail>
        struct unique_types<Head, Tail...> {
            static constexpr bool value =
                !type_list_t<Tail...>::template includes<Head>() && unique_types<Tail...>::value;
        };

        static consteval bool unique()
        {
            if constexpr (sizeof...(Types) == 0)
            {
                return true;
            }
            else
            {
                return unique_types<Types...>::value;
            }
        }

        template<typename T>
        static consteval bool includes()
        {
            return std::disjunction_v<std::is_same<T, Types>...>;
        }

        static consteval bool volatile_types()
        {
            return std::disjunction_v<std::is_volatile<Types>...>;
        }
    };

    template<typename... Types>
    inline constexpr type_list_t<Types...> type_list {};

} // namespace snapfit
