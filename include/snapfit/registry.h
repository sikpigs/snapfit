#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <snapfit/entity.h>
#include <snapfit/query.h>
#include <snapfit/storage.h>

namespace snapfit
{
    template<typename Pool>
    using pool_type = std::remove_pointer_t<Pool>;

    template<typename Pool>
    using pool_component_type = typename std::remove_const_t<pool_type<Pool>>::component_type;

    template<typename Pool>
    using pool_component_reference = std::conditional_t<std::is_const_v<pool_type<Pool>>,
                                                        const pool_component_type<Pool>&,
                                                        pool_component_type<Pool>&>;

    template<typename Entity, typename PoolTuple>
    struct iterator_types;

    template<typename Entity, typename... Pools>
    struct iterator_types<Entity, std::tuple<Pools...>> {
        using value_type = std::tuple<Entity, pool_component_type<Pools>...>;
        using reference = std::tuple<Entity, pool_component_reference<Pools>...>;
    };

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
            using types = iterator_types<entity, ComponentPools>;
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
                return std::apply(
                    [&](auto*... pools) -> reference {
                        return std::tuple<entity, decltype(pools->get(ent))...> {
                            ent, pools->get(ent)...
                        };
                    },
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

                    if (owner->check_liveness)
                    {
                        if (details::entity_index<Traits>(ent) != index)
                        {
                            ++index;
                            continue;
                        }
                    }

                    const bool has_components =
                        std::apply([&](const auto*... pools)
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

        iterator begin() const
        {
            return iterator { this, 0 };
        }

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

    /// Owns entity lifetimes and their type-erased component storages.
    ///
    /// Entity handles contain an index and generation. Releasing an entity
    /// invalidates its handle; a later entity may reuse the index with a newer
    /// generation. The registry is not thread-safe.
    template<traits_type Traits>
    class registry
    {
    public:
        using entity = Traits::entity;

        /// Creates the requested number of entities.
        ///
        /// @throws std::length_error if the range exceeds remaining capacity.
        [[nodiscard]]
        std::vector<entity> create(std::size_t requested)
        {
            const auto capacity = static_cast<std::size_t>(Traits::max_index) + 1;
            const auto remaining = capacity - alive_count - retired_count;

            if (requested > remaining)
            {
                throw std::length_error { "entity index space exhausted" };
            }

            std::vector<entity> hatchery(requested, Traits::null);

            create(hatchery.begin(), hatchery.end());

            return hatchery;
        }

        /// Creates one entity for each element in `[first, last)` and writes
        /// the resulting handles back through the iterators.
        ///
        /// The iterator range must be multipass and mutable. If writing through
        /// an iterator throws, registry state is restored, but elements already
        /// written in the destination range remain modified.
        ///
        /// @throws std::length_error if the range exceeds remaining entity capacity.
        template<typename Iterator>
            requires mutable_entity_iterator<Iterator, entity>
        void create(Iterator first, Iterator last)
        {
            const auto requested = static_cast<std::size_t>(std::ranges::distance(first, last));
            const auto capacity = static_cast<std::size_t>(Traits::max_index) + 1;
            const auto remaining = capacity - alive_count - retired_count;

            if (requested > remaining)
            {
                throw std::length_error { "entity index space exhausted" };
            }

            const auto old_free_head = free_head;
            const auto old_size = entities.size();
            const auto old_alive_count = alive_count;
            const auto free_count = old_size - alive_count - retired_count;
            const auto fresh_count = requested > free_count ? requested - free_count : 0;
            entities.reserve(entities.size() + fresh_count);

            struct previous_head {
                Traits::index_type index;
                entity ent;
            };

            std::vector<previous_head> heads;
            heads.reserve(std::min(free_count, requested));

            auto rollback = [&]()
            {
                entities.resize(old_size);

                std::for_each(
                    heads.rbegin(), heads.rend(), [&](const auto r) { entities[r.index] = r.ent; });

                alive_count = old_alive_count;
                free_head = old_free_head;
            };

            while (free_head != Traits::null_index && first != last)
            {
                try
                {
                    const auto old_head = free_head;
                    const auto old_ent = entities[free_head];
                    const auto ent = reuse();
                    heads.emplace_back(previous_head { old_head, old_ent });
                    *first = ent;
                    ++first;
                }
                catch (...)
                {
                    rollback();
                    throw;
                }
            }

            if (first != last)
            {
                const auto remainder = static_cast<std::size_t>(std::ranges::distance(first, last));
                try
                {
                    entities.reserve(remainder + entities.size());

                    while (first != last)
                    {
                        *first = fresh();
                        ++first;
                    }
                }
                catch (...)
                {
                    rollback();
                    throw;
                }
            }
        }

        /// Creates and returns a live entity.
        ///
        /// Released indices are reused before the entity array is expanded.
        /// @throws std::length_error if every usable entity index is live.
        [[nodiscard]]
        entity create()
        {
            if (free_head != Traits::null_index)
            {
                return reuse();
            }

            if (entities.size() > static_cast<std::size_t>(Traits::max_index))
            {
                throw std::length_error { "entity index space exhausted" };
            }

            return fresh();
        }

        /// Attempts to create the exact hinted entity handle.
        ///
        /// The request fails if the hinted index is outside the usable range or
        /// is already occupied, regardless of the active entity's generation.
        /// Gaps created by an out-of-range hint are added to the free list.
        ///
        /// @return The requested entity on success, or `std::nullopt` on failure.
        [[nodiscard]]
        std::optional<entity> request(entity hint)
        {
            const auto index = details::entity_index<Traits>(hint);

            if (index > Traits::max_index)
            {
                return std::nullopt;
            }

            if (index >= entities.size())
            {
                const auto old_size = static_cast<Traits::index_type>(entities.size());
                entities.resize(static_cast<std::size_t>(index) + 1);

                auto next = free_head;

                for (auto ix = static_cast<std::size_t>(index); ix-- > old_size;)
                {
                    entities[ix] = details::make_entity<Traits>(next, gen_zero);
                    next = static_cast<Traits::index_type>(ix);
                }

                free_head = next;
                entities[index] = hint;
                ++alive_count;

                return hint;
            }

            const auto stored = entities[index];

            if (details::entity_index<Traits>(stored) == index)
            {
                return std::nullopt;
            }

            auto current = free_head;
            auto prev = Traits::null_index;

            while (current != Traits::null_index)
            {
                if (index == current)
                {
                    break;
                }

                const auto ent = entities[current];
                prev = current;
                current = details::entity_index<Traits>(ent);
            }

            if (current == Traits::null_index)
            {
                return std::nullopt;
            }

            const auto next = details::entity_index<Traits>(entities[current]);

            if (prev != Traits::null_index)
            {
                const auto prev_gen = details::entity_generation<Traits>(entities[prev]);
                entities[prev] = details::make_entity<Traits>(next, prev_gen);
            }
            else
            {
                free_head = next;
            }

            ++alive_count;
            entities[current] = hint;
            return hint;
        }

        /// Constructs a component for a live entity and returns it.
        ///
        /// @throws invalid_entity if `ent` is not live.
        /// @throws duplicate_component if `ent` already owns this component type.
        template<typename Component, typename... Arguments>
        decltype(auto) emplace(entity ent, Arguments&&... args)
        {
            if (!valid(ent))
            {
                throw invalid_entity {};
            }

            using component_no_cvref = std::remove_cvref_t<Component>;

            return assure_storage<component_no_cvref>().emplace(ent,
                                                                std::forward<Arguments>(args)...);
        }

        /// Returns a mutable component owned by a live entity.
        ///
        /// @throws invalid_entity if `ent` is not live.
        /// @throws no_such_component if the component does not exist.
        template<typename Component>
            requires(!tag_type<Component>)
        std::remove_cvref_t<Component>& get(entity ent)
        {
            using component_no_cvref = std::remove_cvref_t<Component>;

            return const_cast<component_no_cvref&>(
                std::as_const(*this).template get<component_no_cvref>(ent));
        }

        /// Returns a component owned by a live entity.
        ///
        /// @throws invalid_entity if `ent` is not live.
        /// @throws no_such_component if the component does not exist.
        template<typename Component>
            requires(!tag_type<Component>)
        const std::remove_cvref_t<Component>& get(entity ent) const
        {
            if (!valid(ent))
            {
                throw invalid_entity {};
            }

            using component_no_cvref = std::remove_cvref_t<Component>;
            const auto* pool = find_storage<component_no_cvref>();
            if (!pool)
            {
                throw no_such_component {};
            }

            return pool->get(ent);
        }

        template<typename Component>
            requires(!tag_type<Component>)
        std::remove_cvref_t<Component>* try_get(entity ent)
        {
            using component_no_cvref = std::remove_cvref_t<Component>;

            return const_cast<component_no_cvref*>(
                std::as_const(*this).template try_get<component_no_cvref>(ent));
        }

        template<typename Component>
            requires(!tag_type<Component>)
        const std::remove_cvref_t<Component>* try_get(entity ent) const
        {
            if (!valid(ent))
            {
                return nullptr;
            }

            using component_no_cvref = std::remove_cvref_t<Component>;
            const auto* pool = find_storage<component_no_cvref>();
            if (!pool || !pool->contains(ent))
            {
                return nullptr;
            }

            return std::addressof(pool->get(ent));
        }

        template<typename... Components>
            requires(sizeof...(Components) != 1 && !contains_tag_type<Components...>)
        [[nodiscard]] auto
        try_get(entity ent) const -> std::tuple<const std::remove_cvref_t<Components>*...>
        {
            if (!valid(ent))
            {
                return {};
            }

            return { try_get<Components>(ent)... };
        }

        template<typename... Components>
            requires(sizeof...(Components) != 1 && !contains_tag_type<Components...>)
        [[nodiscard]] auto try_get(entity ent) -> std::tuple<std::remove_cvref_t<Components>*...>
        {
            if (!valid(ent))
            {
                return {};
            }

            return { try_get<Components>(ent)... };
        }

        /// Returns references to zero or multiple components owned by a live entity.
        ///
        /// An empty component list returns `std::tuple<>`.
        /// @throws invalid_entity if `ent` is not live.
        /// @throws no_such_component if any requested component does not exist.
        template<typename... Components>
            requires(sizeof...(Components) != 1 && !contains_tag_type<Components...>)
        [[nodiscard]] auto get(entity ent) -> std::tuple<std::remove_cvref_t<Components>&...>
        {
            if (!valid(ent))
            {
                throw invalid_entity {};
            }

            return { get<Components>(ent)... };
        }

        /// Returns const references to zero or multiple components owned by a live entity.
        ///
        /// An empty component list returns `std::tuple<>`.
        /// @throws invalid_entity if `ent` is not live.
        /// @throws no_such_component if any requested component does not exist.
        template<typename... Components>
            requires(sizeof...(Components) != 1 && !contains_tag_type<Components...>)
        [[nodiscard]] auto
        get(entity ent) const -> std::tuple<const std::remove_cvref_t<Components>&...>
        {
            if (!valid(ent))
            {
                throw invalid_entity {};
            }

            return { get<Components>(ent)... };
        }

        /// Returns whether a live entity owns the requested component type.
        /// Returns `false` for stale or otherwise invalid entity handles.
        template<typename Component>
        bool contains(entity ent) const noexcept
        {
            return valid(ent) && has<Component>(ent);
        }

        /// Removes a component from a live entity.
        ///
        /// @return `true` if a component was removed; otherwise `false`, including
        /// when the entity handle is invalid.
        template<typename Component>
        [[nodiscard]] bool remove(entity ent)
        {
            if (!valid(ent))
            {
                return false;
            }

            using component_no_cvref = std::remove_cvref_t<Component>;
            if (auto* pool = find_storage<component_no_cvref>())
            {
                return pool->erase(ent);
            }

            return false;
        }

        /// Releases an entity and removes all of its components.
        ///
        /// The handle becomes stale and its index may be reused with a newer
        /// generation. Invalid or already released handles are ignored.
        void release(entity ent)
        {
            if (!valid(ent))
            {
                return;
            }

            for (auto& pool : storage_pools)
            {
                if (pool)
                {
                    (void)pool->erase(ent);
                }
            }

            set_free(ent);
        }

        /// Invokes `cb` for every live entity that owns all requested components.
        template<typename... Components, typename Callback>
            requires(
                !contains_tag_type<Components...>
                && std::invocable<Callback&, const entity&, std::remove_cvref_t<Components>&...>)
        void view(Callback&& cb) noexcept(
            std::is_nothrow_invocable_v<Callback&,
                                        const entity&,
                                        std::remove_cvref_t<Components>&...>)
        {
            view<Components...>(include<>, exclude<>, std::forward<Callback>(cb));
        }

        /// Invokes `cb` for every live entity that owns all requested components.
        template<typename... Components, typename... Excluded, typename Callback>
            requires(
                !contains_tag_type<Components...>
                && std::invocable<Callback&, const entity&, std::remove_cvref_t<Components>&...>)
        void view(exclude_t<Excluded...>, Callback&& cb) noexcept(
            std::is_nothrow_invocable_v<Callback&,
                                        const entity&,
                                        std::remove_cvref_t<Components>&...>)
        {
            view<Components...>(include<>, exclude<Excluded...>, std::forward<Callback>(cb));
        }

        template<typename... Components,
                 typename... Included,
                 typename... Excluded,
                 typename Callback>
            requires(
                !contains_tag_type<Components...>
                && std::invocable<Callback&, const entity&, std::remove_cvref_t<Components>&...>)
        void view(include_t<Included...>, exclude_t<Excluded...>, Callback&& cb) noexcept(
            std::is_nothrow_invocable_v<Callback&,
                                        const entity&,
                                        std::remove_cvref_t<Components>&...>)
        {
            view_impl(
                *this, type_list<Components...>, include<Included...>, exclude<Excluded...>, cb);
        }

        /// Invokes `cb` for every live entity that owns all requested components.
        template<typename... Components, typename Callback>
            requires(!contains_tag_type<Components...>
                     && std::invocable<Callback&,
                                       const entity&,
                                       const std::remove_cvref_t<Components>&...>)
        void view(Callback&& cb) const
            noexcept(std::is_nothrow_invocable_v<Callback&,
                                                 const entity&,
                                                 const std::remove_cvref_t<Components>&...>)
        {
            view<Components...>(include<>, exclude<>, std::forward<Callback>(cb));
        }

        /// Invokes `cb` for every matching entity that owns no excluded component.
        template<typename... Components, typename... Excluded, typename Callback>
            requires(!contains_tag_type<Components...>
                     && std::invocable<Callback&,
                                       const entity&,
                                       const std::remove_cvref_t<Components>&...>)
        void view(exclude_t<Excluded...>, Callback&& cb) const
            noexcept(std::is_nothrow_invocable_v<Callback&,
                                                 const entity&,
                                                 const std::remove_cvref_t<Components>&...>)
        {
            view<Components...>(include<>, exclude<Excluded...>, std::forward<Callback>(cb));
        }

        template<typename... Components,
                 typename... Included,
                 typename... Excluded,
                 typename Callback>
            requires(!contains_tag_type<Components...>
                     && std::invocable<Callback&,
                                       const entity&,
                                       const std::remove_cvref_t<Components>&...>)
        void view(include_t<Included...>, exclude_t<Excluded...>, Callback&& cb) const
            noexcept(std::is_nothrow_invocable_v<Callback&,
                                                 const entity&,
                                                 const std::remove_cvref_t<Components>&...>)
        {
            view_impl(
                *this, type_list<Components...>, include<Included...>, exclude<Excluded...>, cb);
        }

        template<typename... Components, typename... Included, typename... Excluded>
            requires(!contains_tag_type<Components...>)
        auto view(include_t<Included...>, exclude_t<Excluded...>)
        {
            return view_impl(
                *this, type_list<Components...>, include<Included...>, exclude<Excluded...>);
        }

        template<typename... Components, typename... Excluded>
            requires(!contains_tag_type<Components...>)
        auto view(exclude_t<Excluded...>)
        {
            return view_impl(*this, type_list<Components...>, include<>, exclude<Excluded...>);
        }

        template<typename... Components>
            requires(!contains_tag_type<Components...>)
        auto view()
        {
            return view_impl(*this, type_list<Components...>, include<>, exclude<>);
        }

        template<typename... Components, typename... Included, typename... Excluded>
            requires(!contains_tag_type<Components...>)
        auto view(include_t<Included...>, exclude_t<Excluded...>) const
        {
            return view_impl(
                *this, type_list<Components...>, include<Included...>, exclude<Excluded...>);
        }

        template<typename... Components, typename... Excluded>
            requires(!contains_tag_type<Components...>)
        auto view(exclude_t<Excluded...>) const
        {
            return view_impl(*this, type_list<Components...>, include<>, exclude<Excluded...>);
        }

        template<typename... Components>
            requires(!contains_tag_type<Components...>)
        auto view() const
        {
            return view_impl(*this, type_list<Components...>, include<>, exclude<>);
        }

    private:
        static constexpr auto gen_zero = static_cast<Traits::generation_type>(0);
        std::vector<entity> entities;
        Traits::index_type free_head = Traits::null_index;
        std::size_t alive_count {};
        std::size_t retired_count {};
        std::vector<std::unique_ptr<storage_base<entity>>> storage_pools;

        [[nodiscard]]
        entity fresh()
        {
            const auto index = static_cast<Traits::index_type>(entities.size());
            const auto value = details::make_entity<Traits>(index, gen_zero);

            entities.push_back(value);
            ++alive_count;

            return value;
        }

        bool valid(entity ent) const noexcept
        {
            const auto index = details::entity_index<Traits>(ent);
            return index < entities.size() && entities[index] == ent;
        }

        void set_free(entity ent) noexcept
        {
            const auto index = details::entity_index<Traits>(ent);
            const auto gen = details::entity_generation<Traits>(ent);

            if (gen != Traits::max_generation)
            {
                const auto next_gen = static_cast<Traits::generation_type>(gen + 1);
                entities[index] = details::make_entity<Traits>(free_head, next_gen);
                free_head = index;
                --alive_count;
                return;
            }

            if constexpr (std::is_same_v<typename Traits::wrap_policy, wrap_generation>)
            {
                entities[index] = details::make_entity<Traits>(free_head, gen_zero);
                free_head = index;
            }
            else if constexpr (std::is_same_v<typename Traits::wrap_policy, tombstone_on_wrap>)
            {
                entities[index] =
                    details::make_entity<Traits>(Traits::null_index, Traits::tombstone);
                ++retired_count;
            }
            else
            {
                assert(false && "entity generation exhausted");
                std::terminate();
            }

            --alive_count;
        }

        entity reuse() noexcept
        {
            const auto index = free_head;
            const auto free_node = entities[index];

            free_head = details::entity_index<Traits>(free_node);

            const auto generation = details::entity_generation<Traits>(free_node);
            const auto value = details::make_entity<Traits>(index, generation);

            entities[index] = value;
            ++alive_count;

            return value;
        }

        template<typename Component>
        storage<Component, Traits>& assure_storage()
        {
            const auto id = details::id_of<Component>();
            if (id >= storage_pools.size())
            {
                storage_pools.resize(id + 1);
            }

            if (!storage_pools[id])
            {
                storage_pools[id] = std::make_unique<storage<Component, Traits>>();
            }

            return static_cast<storage<Component, Traits>&>(*storage_pools[id]);
        }

        template<typename Component>
        const storage<Component, Traits>* find_storage() const noexcept
        {
            const auto id = details::id_of<Component>();

            if (id >= storage_pools.size() || !storage_pools[id])
            {
                return nullptr;
            }

            return static_cast<const storage<Component, Traits>*>(storage_pools[id].get());
        }

        template<typename Component>
        storage<Component, Traits>* find_storage() noexcept
        {
            return const_cast<storage<Component, Traits>*>(
                std::as_const(*this).template find_storage<Component>());
        }

        template<typename... Components>
        auto pools_with() const noexcept
            -> std::optional<std::array<const storage_base<entity>*, sizeof...(Components)>>
        {
            std::array pools { static_cast<const storage_base<entity>*>(
                find_storage<Components>())... };
            if (std::ranges::find(pools, nullptr) != pools.end())
            {
                return std::nullopt;
            }

            return pools;
        }

        template<typename Component>
        [[nodiscard]]
        bool has(entity ent) const noexcept
        {
            using component_no_cvref = std::remove_cvref_t<Component>;
            const auto* pool = find_storage<component_no_cvref>();
            return pool && pool->contains(ent);
        }

        template<typename Self, typename Component>
        using view_component_ref_t = std::conditional_t<std::is_const_v<Self>,
                                                        const std::remove_cvref_t<Component>&,
                                                        std::remove_cvref_t<Component>&>;

        template<typename Self,
                 typename... Components,
                 typename... Included,
                 typename... Excluded,
                 typename Callback>
        static void view_impl(
            Self& self,
            type_list_t<Components...>,
            include_t<Included...>,
            exclude_t<Excluded...>,
            Callback&&
                cb) noexcept(std::is_nothrow_invocable_v<Callback&,
                                                         const entity&,
                                                         view_component_ref_t<Self, Components>...>)
        {
            auto component_pools =
                std::tuple { self.template find_storage<std::remove_cvref_t<Components>>()... };

            std::array<const storage_base<entity>*, sizeof...(Included)> included_pools {
                self.template find_storage<std::remove_cvref_t<Included>>()...
            };

            std::array<const storage_base<entity>*, sizeof...(Excluded)> excluded_pools {
                self.template find_storage<std::remove_cvref_t<Excluded>>()...
            };

            const auto visit = [&](const entity ent)
            {
                const auto has_components =
                    std::apply([&](const auto*... pools) { return (pools->contains(ent) && ...); },
                               component_pools);
                const auto has_includes = std::ranges::all_of(
                    included_pools, [&](const auto* pool) { return pool->contains(ent); });
                const auto is_excluded = std::ranges::any_of(
                    excluded_pools, [&](const auto* pool) { return pool && pool->contains(ent); });

                if (!has_components || !has_includes || is_excluded)
                {
                    return;
                }

                std::apply([&](auto*... pools) { std::invoke(cb, ent, pools->get(ent)...); },
                           component_pools);
            };

            if constexpr (sizeof...(Components) + sizeof...(Included) == 0)
            {
                for (std::size_t index = 0; index < self.entities.size(); ++index)
                {
                    const auto ent = self.entities[index];
                    if (details::entity_index<Traits>(ent) == index)
                    {
                        visit(ent);
                    }
                }
            }
            else
            {
                auto pools = self.template pools_with<std::remove_cvref_t<Components>...,
                                                      std::remove_cvref_t<Included>...>();

                if (!pools)
                {
                    return;
                }

                const auto smallest = std::ranges::min_element(
                    *pools, {}, [](const auto* pool) { return pool->size(); });

                for (const auto ent : (*smallest)->entities())
                {
                    visit(ent);
                }
            }
        }

        template<typename Self, typename... Components, typename... Included, typename... Excluded>
            requires(!contains_tag_type<Components...>)
        static auto view_impl(Self& self,
                              type_list_t<Components...>,
                              include_t<Included...>,
                              exclude_t<Excluded...>)
        {
            auto component_pools =
                std::tuple { self.template find_storage<std::remove_cvref_t<Components>>()... };

            std::array<const storage_base<entity>*, sizeof...(Included)> included_pools {
                self.template find_storage<std::remove_cvref_t<Included>>()...
            };

            std::array<const storage_base<entity>*, sizeof...(Excluded)> excluded_pools {
                self.template find_storage<std::remove_cvref_t<Excluded>>()...
            };

            if constexpr (sizeof...(Components) + sizeof...(Included) == 0)
            {
                return basic_view<Traits,
                                  decltype(component_pools),
                                  sizeof...(Included),
                                  sizeof...(Excluded)>(std::move(component_pools),
                                                       std::move(included_pools),
                                                       std::move(excluded_pools),
                                                       self.entities,
                                                       true);
            }
            else
            {
                auto contains_nullptr = [&]<std::size_t... Is>(std::index_sequence<Is...>)
                { return ((std::get<Is>(component_pools) == nullptr) || ...); };

                bool has_missing_pools =
                    contains_nullptr(std::make_index_sequence<sizeof...(Components)> {});

                has_missing_pools |= std::ranges::any_of(
                    included_pools, [](const auto* pool) { return pool == nullptr; });

                if (has_missing_pools)
                {
                    return basic_view<Traits,
                                      decltype(component_pools),
                                      sizeof...(Included),
                                      sizeof...(Excluded)> {};
                }

                std::array<const storage_base<entity>*, sizeof...(Components) + sizeof...(Included)>
                    pools { self.template find_storage<std::remove_cvref_t<Components>>()...,
                            self.template find_storage<std::remove_cvref_t<Included>>()... };

                const auto smallest = std::ranges::min_element(
                    pools, {}, [](const auto* pool) { return pool->size(); });

                return basic_view<Traits,
                                  decltype(component_pools),
                                  sizeof...(Included),
                                  sizeof...(Excluded)>(std::move(component_pools),
                                                       std::move(included_pools),
                                                       std::move(excluded_pools),
                                                       (*smallest)->entities(),
                                                       false);
            }
        }
    };

} // namespace snapfit
