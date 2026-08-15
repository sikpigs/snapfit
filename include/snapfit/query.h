#pragma once

#include <type_traits>

#include <snapfit/component.h>

namespace snapfit
{
    /// Identifies component types that an entity must own without returning them.
    template<typename... Components>
    struct include_t { };

    /// Query argument requiring the listed component types.
    template<typename... Components>
    inline constexpr include_t<Components...> include {};

    /// Identifies component types that an entity must not own.
    template<typename... Components>
    struct exclude_t { };

    /// Query argument rejecting entities that own any listed component type.
    template<typename... Components>
    inline constexpr exclude_t<Components...> exclude {};

    /// Identifies component types returned as nullable pointers without filtering entities.
    template<typename... Components>
    struct optional_t { };

    /// Query argument returning the listed component types as nullable pointers.
    template<typename... Components>
    inline constexpr optional_t<Components...> optional {};

    /// A compile-time list used to preserve component parameter packs.
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
    };

    /// Compile-time value representing a list of types.
    template<typename... Types>
    inline constexpr type_list_t<Types...> type_list {};

    /// Constrains view result components to unique, non-tag, non-volatile types.
    template<typename... Components>
    concept view_component_types =
        !contains_tag_type<Components...>
        && type_list_t<std::remove_cvref_t<Components>...>::unique()
        && !(std::is_volatile_v<std::remove_reference_t<Components>> || ...);

    namespace details
    {
        template<typename Component>
        struct optional_component {
            using type = Component;
        };

        template<typename T>
        struct is_optional_component : std::false_type { };

        template<typename T>
        struct is_optional_component<optional_component<T>> : std::true_type { };
    } // namespace details

} // namespace snapfit
