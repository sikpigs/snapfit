#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <snapfit/entity.h>
#include <snapfit/query.h>
#include <snapfit/storage.h>
#include <snapfit/view.h>

namespace snapfit
{
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
            entity_components.reserve(entity_components.size() + fresh_count);

            struct previous_head {
                Traits::index_type index;
                entity ent;
            };

            std::vector<previous_head> heads;
            heads.reserve(std::min(free_count, requested));

            auto rollback = [&]()
            {
                entities.resize(old_size);
                entity_components.resize(old_size);

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
                    entity_components.reserve(remainder + entity_components.size());

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

                try
                {
                    entity_components.resize(static_cast<std::size_t>(index + 1));
                }
                catch (...)
                {
                    entities.resize(old_size);
                    throw;
                }

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
            entity_components[current] = component_membership {};
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

            const auto id = details::id_of<component_no_cvref>();
            auto& membership =
                entity_components[static_cast<std::size_t>(details::entity_index<Traits>(ent))];
            if (membership.inline_size == Traits::component_cache_size)
            {
                membership.overflow.push_back(id);
            }
            else
            {
                membership.inline_ids[membership.inline_size++] = id;
            }

            try
            {
                return assure_storage<component_no_cvref>().emplace(
                    ent, std::forward<Arguments>(args)...);
            }
            catch (...)
            {
                if (!membership.overflow.empty())
                {
                    membership.overflow.pop_back();
                }
                else
                {
                    --membership.inline_size;
                }

                throw;
            }
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
            const auto* pool = try_find_storage<component_no_cvref>();
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
            const auto* pool = try_find_storage<component_no_cvref>();
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

            auto& membership = entity_components[details::entity_index<Traits>(ent)];

            using component_no_cvref = std::remove_cvref_t<Component>;

            auto id = details::id_of<component_no_cvref>();

            for (std::size_t ix = 0; ix < membership.inline_size; ++ix)
            {
                if (membership.inline_ids[ix] == id)
                {
                    const auto erase_result =
                        storage_pools[id] ? storage_pools[id]->erase(ent) : false;

                    std::size_t jx = ix;
                    if (!membership.overflow.empty())
                    {
                        membership.inline_ids[jx] = membership.overflow.back();
                        membership.overflow.pop_back();
                    }
                    else
                    {
                        for (; jx < membership.inline_size - 1; ++jx)
                        {
                            membership.inline_ids[jx] = membership.inline_ids[jx + 1];
                        }
                        --membership.inline_size;
                    }

                    return erase_result;
                }
            }

            for (auto it = membership.overflow.begin(); it != membership.overflow.end(); ++it)
            {
                if (*it == id)
                {
                    const auto erase_result =
                        storage_pools[id] ? storage_pools[id]->erase(ent) : false;
                    membership.overflow.erase(it);
                    return erase_result;
                }
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

            erase(ent);
        }

        /// Releases every live entity in `[first, last)` and removes its components.
        ///
        /// The input range is copied before registry state is modified, so it may be
        /// single-pass or backed by component storage that release invalidates. Stale handles
        /// are ignored, and duplicate handles release an entity only once. Released handles
        /// become invalid and their indices may be reused with newer generations.
        template<std::input_iterator Iterator, std::sentinel_for<Iterator> Sentinel>
            requires std::same_as<std::remove_cv_t<std::iter_value_t<Iterator>>, entity>
        void release(Iterator first, Sentinel last)
        {
            std::vector<entity> pending {};

            for (; first != last; ++first)
            {
                const auto ent = *first;
                if (valid(ent))
                {
                    pending.push_back(ent);
                }
            }

            std::vector<std::vector<entity>> removals(storage_pools.size());

            for (const auto ent : pending)
            {
                const auto index = details::entity_index<Traits>(ent);
                const auto& membership = entity_components[index];
                for (std::size_t ix = 0; ix < membership.inline_size; ++ix)
                {
                    const auto id = membership.inline_ids[ix];
                    removals[id].push_back(ent);
                }

                for (const auto id : membership.overflow)
                {
                    removals[id].push_back(ent);
                }
            }

            for (std::size_t id = 0; id < removals.size(); ++id)
            {
                auto* pool = storage_pools[id].get();
                if (!pool)
                {
                    continue;
                }

                for (const auto ent : removals[id])
                {
                    (void)pool->erase(ent);
                }
            }

            for (const auto ent : pending)
            {
                if (valid(ent))
                {
                    set_free(ent);
                }
            }
        }

        /// Returns a view yielding `Components...` for entities that satisfy the given filters.
        /// Included types must be present and excluded types must be absent; filter components
        /// are not returned. A mutable registry yields mutable references unless a component is
        /// const.
        template<typename... Components, typename... Included, typename... Excluded>
            requires view_component_types<Components...>
        auto view(include_t<Included...>, exclude_t<Excluded...>)
        {
            return view_impl(*this,
                             type_list<Components...>,
                             include<Included...>,
                             exclude<Excluded...>,
                             optional<>);
        }

        /// Returns a view yielding `Components...` while rejecting excluded component types.
        template<typename... Components, typename... Excluded>
            requires view_component_types<Components...>
        auto view(exclude_t<Excluded...>)
        {
            return view_impl(
                *this, type_list<Components...>, include<>, exclude<Excluded...>, optional<>);
        }

        /// Returns a view yielding `Components...` for entities that own every requested type.
        /// An empty component list visits every live entity.
        template<typename... Components>
            requires view_component_types<Components...>
        auto view()
        {
            return view_impl(*this, type_list<Components...>, include<>, exclude<>, optional<>);
        }

        /// Returns a const view yielding `Components...` for entities satisfying the filters.
        /// Every returned component reference is const regardless of the requested
        /// cv-qualification.
        template<typename... Components, typename... Included, typename... Excluded>
            requires view_component_types<Components...>
        auto view(include_t<Included...>, exclude_t<Excluded...>) const
        {
            return view_impl(*this,
                             type_list<Components...>,
                             include<Included...>,
                             exclude<Excluded...>,
                             optional<>);
        }

        /// Returns a const view yielding components while rejecting excluded component types.
        template<typename... Components, typename... Excluded>
            requires view_component_types<Components...>
        auto view(exclude_t<Excluded...>) const
        {
            return view_impl(
                *this, type_list<Components...>, include<>, exclude<Excluded...>, optional<>);
        }

        /// Returns a const view for entities that own every requested component type.
        /// An empty component list visits every live entity.
        template<typename... Components>
            requires view_component_types<Components...>
        auto view() const
        {
            return view_impl(*this, type_list<Components...>, include<>, exclude<>, optional<>);
        }

        /// Returns a const view with optional components represented by nullable const
        /// pointers. Optional component types do not affect whether an entity matches.
        template<typename... Components, typename... Optional>
            requires view_component_types<Components..., Optional...>
        auto view(optional_t<Optional...>) const
        {
            return view_impl(
                *this, type_list<Components...>, include<>, exclude<>, optional<Optional...>);
        }

        /// Returns a mutable view with optional components represented by nullable pointers.
        /// Optional component types do not affect whether an entity matches.
        template<typename... Components, typename... Optional>
            requires view_component_types<Components..., Optional...>
        auto view(optional_t<Optional...>)
        {
            return view_impl(
                *this, type_list<Components...>, include<>, exclude<>, optional<Optional...>);
        }

        /// Returns a mutable view with excluded filters and nullable optional components.
        template<typename... Components, typename... Excluded, typename... Optional>
            requires view_component_types<Components..., Optional...>
        auto view(exclude_t<Excluded...>, optional_t<Optional...>)
        {
            return view_impl(*this,
                             type_list<Components...>,
                             include<>,
                             exclude<Excluded...>,
                             optional<Optional...>);
        }

        /// Returns a const view with excluded filters and nullable optional components.
        template<typename... Components, typename... Excluded, typename... Optional>
            requires view_component_types<Components..., Optional...>
        auto view(exclude_t<Excluded...>, optional_t<Optional...>) const
        {
            return view_impl(*this,
                             type_list<Components...>,
                             include<>,
                             exclude<Excluded...>,
                             optional<Optional...>);
        }

        /// Returns a mutable view with required, included, excluded, and optional components.
        template<typename... Components,
                 typename... Included,
                 typename... Excluded,
                 typename... Optional>
            requires view_component_types<Components..., Optional...>
        auto view(include_t<Included...>, exclude_t<Excluded...>, optional_t<Optional...>)
        {
            return view_impl(*this,
                             type_list<Components...>,
                             include<Included...>,
                             exclude<Excluded...>,
                             optional<Optional...>);
        }

        /// Returns a const view with required, included, excluded, and optional components.
        template<typename... Components,
                 typename... Included,
                 typename... Excluded,
                 typename... Optional>
            requires view_component_types<Components..., Optional...>
        auto view(include_t<Included...>, exclude_t<Excluded...>, optional_t<Optional...>) const
        {
            return view_impl(*this,
                             type_list<Components...>,
                             include<Included...>,
                             exclude<Excluded...>,
                             optional<Optional...>);
        }

    private:
        template<auto V>
        struct smallest_int_for_value {
            using type = std::conditional_t<
                (V <= 0xFF),
                std::uint8_t,
                std::conditional_t<
                    (V <= 0xFFFF),
                    std::uint16_t,
                    std::conditional_t<(V <= 0xFFFFFFFF), std::uint32_t, std::uint64_t>>>;
        };

        template<auto V>
        using smallest_int_for_value_t = typename smallest_int_for_value<V>::type;

        struct component_membership {
            std::array<details::type_id, Traits::component_cache_size> inline_ids {};
            std::vector<details::type_id> overflow {};
            smallest_int_for_value_t<Traits::component_cache_size> inline_size {};
        };

        static constexpr auto gen_zero = static_cast<Traits::generation_type>(0);
        std::vector<entity> entities;
        std::vector<component_membership> entity_components;
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

            try
            {
                entity_components.emplace_back();
            }
            catch (...)
            {
                entities.pop_back();
                throw;
            }

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
            entity_components[index] = component_membership {};
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
        const storage<Component, Traits>* try_find_storage() const noexcept
        {
            const auto id = details::id_of<Component>();

            if (id >= storage_pools.size() || !storage_pools[id])
            {
                return nullptr;
            }

            return static_cast<const storage<Component, Traits>*>(storage_pools[id].get());
        }

        template<typename Component>
        storage<Component, Traits>* try_find_storage() noexcept
        {
            return const_cast<storage<Component, Traits>*>(
                std::as_const(*this).template try_find_storage<Component>());
        }

        template<typename Component>
        [[nodiscard]] bool has(entity ent) const noexcept
        {
            using component_no_cvref = std::remove_cvref_t<Component>;
            const auto& membership =
                entity_components[static_cast<std::size_t>(details::entity_index<Traits>(ent))];

            auto id = details::id_of<component_no_cvref>();
            for (std::size_t ix = 0; ix < membership.inline_size; ++ix)
            {
                if (membership.inline_ids[ix] == id)
                {
                    return true;
                }
            }

            const auto* pool = try_find_storage<component_no_cvref>();
            return pool && pool->contains(ent);
        }

        void erase(entity ent)
        {
            auto erase_from = [&](auto& pool)
            {
                if (pool)
                {
                    pool->erase(ent);
                }
            };

            auto& membership =
                entity_components[static_cast<std::size_t>(details::entity_index<Traits>(ent))];
            for (std::size_t ix = 0; ix < membership.inline_size; ++ix)
            {
                erase_from(storage_pools[membership.inline_ids[ix]]);
            }

            for (auto ix : membership.overflow)
            {
                erase_from(storage_pools[ix]);
            }

            set_free(ent);
        }

        template<typename Component>
        struct get_component {
            using type = Component;
        };

        template<typename Component>
        struct get_component<details::optional_component<Component>> {
            using type = typename details::optional_component<Component>::type;
        };

        template<typename Component>
        using get_component_t = typename get_component<Component>::type;

        template<typename Component, typename Self>
        static auto view_storage(Self& self) noexcept
        {
            using component_type = get_component_t<Component>;
            constexpr bool is_const =
                std::is_const_v<Self> || std::is_const_v<std::remove_reference_t<component_type>>;
            constexpr bool is_opt = details::is_optional_component<Component>::value;
            using component_no_cvref = std::remove_cvref_t<component_type>;

            if constexpr (is_const)
            {
                const auto* pool = static_cast<const storage<component_no_cvref, Traits>*>(
                    self.template try_find_storage<component_no_cvref>());
                if constexpr (is_opt)
                {
                    return details::optional_storage<std::remove_pointer_t<decltype(pool)>> {
                        .pool = pool
                    };
                }
                else
                {
                    return pool;
                }
            }
            else
            {
                auto* pool = self.template try_find_storage<component_no_cvref>();
                if constexpr (is_opt)
                {
                    return details::optional_storage<std::remove_pointer_t<decltype(pool)>> {
                        .pool = pool
                    };
                }
                else
                {
                    return pool;
                }
            }
        }

        template<typename Self,
                 typename... Components,
                 typename... Included,
                 typename... Excluded,
                 typename... Optional>
        static auto view_impl(Self& self,
                              type_list_t<Components...>,
                              include_t<Included...>,
                              exclude_t<Excluded...>,
                              optional_t<Optional...>)
        {
            auto component_pools =
                std::tuple { view_storage<Components>(self)...,
                             view_storage<details::optional_component<Optional>>(self)... };

            std::array<const storage_base<entity>*, sizeof...(Included)> included_pools {
                self.template try_find_storage<std::remove_cvref_t<Included>>()...
            };

            std::array<const storage_base<entity>*, sizeof...(Excluded)> excluded_pools {
                self.template try_find_storage<std::remove_cvref_t<Excluded>>()...
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
                    pools { self.template try_find_storage<std::remove_cvref_t<Components>>()...,
                            self.template try_find_storage<std::remove_cvref_t<Included>>()... };

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
