#pragma once

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
    struct type_list_t { };

    template<typename... Types>
    inline constexpr type_list_t<Types...> type_list {};

} // namespace snapfit
