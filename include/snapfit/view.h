#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

#include <snapfit/entity.h>
#include <snapfit/storage.h>

namespace snapfit
{
    namespace details
    {
        template<typename Storage>
        struct optional_storage {
            using storage_type = std::remove_const_t<Storage>;
            using component_type = typename storage_type::component_type;
            using entity = typename storage_type::entity;

            Storage* pool = nullptr;

            constexpr operator bool() const noexcept
            {
                return true;
            }

            constexpr bool contains(entity) const noexcept
            {
                return true;
            }

            auto get(entity ent) const -> std::conditional_t<std::is_const_v<Storage>,
                                                             const component_type*,
                                                             component_type*>
            {
                if (pool && pool->contains(ent))
                {
                    return std::addressof(pool->get(ent));
                }

                return nullptr;
            }

            const optional_storage* operator->() const noexcept
            {
                return this;
            }
        };

        template<typename Pool>
        struct pool_type {
            using type = std::remove_pointer_t<Pool>;
        };

        template<typename Pool>
        struct pool_type<optional_storage<Pool>> {
            using type = typename optional_storage<Pool>::storage_type;
        };

        template<typename Pool>
        using pool_type_t = typename pool_type<Pool>::type;

        template<typename Pool>
        using pool_component_type_t =
            typename std::remove_const_t<pool_type_t<Pool>>::component_type;

        template<typename Pool>
        struct pool_component_reference {
            using type = std::conditional_t<std::is_const_v<pool_type_t<Pool>>,
                                            const pool_component_type_t<Pool>&,
                                            pool_component_type_t<Pool>&>;
        };

        template<typename Pool>
        struct pool_component_reference<optional_storage<Pool>> {
            using type = std::conditional_t<std::is_const_v<pool_type_t<Pool>>,
                                            const pool_component_type_t<Pool>*,
                                            pool_component_type_t<Pool>*>;
        };

        template<typename Pool>
        using pool_component_reference_t = typename pool_component_reference<Pool>::type;

        template<typename Pool>
        struct pool_component_value {
            using type = pool_component_type_t<Pool>;
        };

        template<typename Pool>
        struct pool_component_value<optional_storage<Pool>> {
            using type = pool_component_reference_t<optional_storage<Pool>>;
        };

        template<typename Pool>
        using pool_component_value_t = typename pool_component_value<Pool>::type;

        template<typename Entity, typename PoolTuple>
        struct iterator_types;

        template<typename Entity, typename... Pools>
        struct iterator_types<Entity, std::tuple<Pools...>> {
            using value_type = std::tuple<Entity, pool_component_value_t<Pools>...>;
            using reference = std::tuple<Entity, pool_component_reference_t<Pools>...>;
        };
    } // namespace details

    /// A non-owning input range over entities and their requested components.
    ///
    /// Views are invalidated when registry operations invalidate their candidate entity span or
    /// component storage pointers. Required components are yielded by reference; optional
    /// components are yielded as nullable pointers.
    template<traits_type Traits,
             typename ComponentPools,
             std::size_t IncludedCount,
             std::size_t ExcludedCount>
    class basic_view : public std::ranges::view_interface<
                           basic_view<Traits, ComponentPools, IncludedCount, ExcludedCount>>
    {
    public:
        using entity = Traits::entity;

        class iterator
        {
        public:
            using difference_type = std::ptrdiff_t;
            using iterator_concept = std::input_iterator_tag;
            using iterator_category = std::input_iterator_tag;
            using types = details::iterator_types<entity, ComponentPools>;
            using value_type = typename types::value_type;
            using reference = typename types::reference;

            iterator() = default;

            iterator(const basic_view* owning_view, std::size_t start_index)
                : owner { owning_view }
                , index { start_index }
            {
                satisfy();
            }

            reference operator*() const
            {
                assert(owner != nullptr);
                assert(index < owner->candidates.size());

                const auto ent = owner->candidates[index];
                return std::apply([&](const auto&... pools) -> reference
                                  { return { ent, pools->get(ent)... }; },
                                  owner->component_pools);
            }

            iterator& operator++()
            {
                ++index;
                satisfy();
                return *this;
            }

            iterator operator++(int)
            {
                auto prev = *this;
                ++*this;
                return prev;
            }

            friend bool operator==(const iterator&, const iterator&) = default;

        private:
            void satisfy()
            {
                while (index < owner->candidates.size())
                {
                    const auto ent = owner->candidates[index];

                    if (owner->check_liveness && details::entity_index<Traits>(ent) != index)
                    {
                        ++index;
                        continue;
                    }

                    const bool has_components =
                        std::apply([&](const auto&... pools)
                                   { return ((pools && pools->contains(ent)) && ...); },
                                   owner->component_pools);
                    if (!has_components)
                    {
                        ++index;
                        continue;
                    }

                    const bool has_includes = std::ranges::all_of(
                        owner->included_pools,
                        [&](const auto* pool) { return pool && pool->contains(ent); });

                    if (!has_includes)
                    {
                        ++index;
                        continue;
                    }

                    const bool is_excluded = std::ranges::any_of(
                        owner->excluded_pools,
                        [&](const auto* pool) { return pool && pool->contains(ent); });

                    if (is_excluded)
                    {
                        ++index;
                        continue;
                    }

                    break;
                }
            }

            const basic_view* owner = nullptr;
            std::size_t index = 0;
        };

        using included_pools_t = std::array<const storage_base<entity>*, IncludedCount>;
        using excluded_pools_t = std::array<const storage_base<entity>*, ExcludedCount>;
        using candidates_t = std::span<const entity>;

        basic_view() = default;

        basic_view(ComponentPools&& component_pools_in,
                   included_pools_t&& included_pools_in,
                   excluded_pools_t&& excluded_pools_in,
                   candidates_t candidates_in,
                   bool check_liveness_in)
            : component_pools { std::forward<ComponentPools>(component_pools_in) }
            , included_pools { std::forward<included_pools_t>(included_pools_in) }
            , excluded_pools { std::forward<excluded_pools_t>(excluded_pools_in) }
            , candidates { candidates_in }
            , check_liveness { check_liveness_in }
        {
        }

        /// Returns an iterator positioned at the first matching entity.
        iterator begin() const
        {
            return iterator { this, 0 };
        }

        /// Returns the sentinel iterator following the last candidate entity.
        iterator end() const
        {
            return iterator { this, candidates.size() };
        }

    private:
        ComponentPools component_pools {};
        included_pools_t included_pools {};
        excluded_pools_t excluded_pools {};
        candidates_t candidates {};
        bool check_liveness { false };
    };
} // namespace snapfit
