#pragma once

#include <type_traits>

namespace snapfit
{
    template<typename T>
    struct tag {
        using type = std::remove_cvref_t<T>;
    };

    template<typename Type>
    struct is_tag : std::false_type { };

    template<typename Type>
    struct is_tag<tag<Type>> : std::true_type { };

    template<typename Type>
    concept tag_type = is_tag<std::remove_cvref_t<Type>>::value;

    template<typename... Types>
    concept contains_tag_type = (tag_type<Types> || ...);

} // namespace snapfit
