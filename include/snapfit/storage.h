#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <snapfit/component.h>
#include <snapfit/entity.h>

namespace snapfit
{
    template<entity_type Entity>
    class storage_base
    {
    public:
        storage_base() = default;
        storage_base(const storage_base&) = delete;
        storage_base(storage_base&&) = delete;
        storage_base& operator=(const storage_base&) = delete;
        storage_base& operator=(storage_base&&) = delete;

        virtual ~storage_base() = default;

        [[nodiscard]] virtual bool erase(Entity ent) = 0;
        virtual void clear() noexcept = 0;
        [[nodiscard]] virtual bool contains(Entity ent) const noexcept = 0;
        [[nodiscard]] virtual std::size_t size() const noexcept = 0;
        [[nodiscard]] virtual std::span<const Entity> entities() const noexcept = 0;
    };

    template<typename Component>
    class payload
    {
    public:
        template<typename... Arguments>
        Component& emplace(Arguments&&... args)
        {
            return components.emplace_back(std::forward<Arguments>(args)...);
        }

        void replace_with_last(std::size_t replace_at)
        {
            const auto last = components.size() - 1;
            if (replace_at != last)
            {
                if constexpr (std::is_nothrow_move_assignable_v<Component>)
                {
                    components[replace_at] = std::move(components[last]);
                }
                else
                {
                    static_assert(std::is_nothrow_swappable_v<Component>);

                    std::ranges::swap(components[replace_at], components[last]);
                }
            }
        }

        void pop_back() noexcept
        {
            components.pop_back();
        }

        Component& operator[](std::size_t index)
        {
            return components[index];
        }

        const Component& operator[](std::size_t index) const
        {
            return components[index];
        }

        void clear() noexcept
        {
            components.clear();
        }

    private:
        std::vector<Component> components;
    };

    template<typename Type>
    class payload<tag<Type>>
    {
    public:
        void emplace() noexcept
        {
        }

        void replace_with_last(std::size_t) noexcept
        {
        }

        void pop_back() noexcept
        {
        }

        void clear() noexcept
        {
        }
    };

    /// Sparse-set storage for one component type.
    ///
    /// Component references can be invalidated by insertion, erasure, or clear.
    template<typename Component, traits_type Traits>
    class storage final : public storage_base<typename Traits::entity>
    {
    public:
        using entity = typename Traits::entity;
        using size_type = std::size_t;
        using component_type = Component;

        /// Constructs a component for `ent` and returns it.
        ///
        /// @throws duplicate_component if the entity already has a component.
        template<typename... Arguments>
        decltype(auto) emplace(entity ent, Arguments&&... args)
        {
            const auto index = static_cast<size_type>(details::entity_index<Traits>(ent));

            if (index >= sparse.size())
            {
                sparse.resize(index + 1, invalid);
            }

            if (sparse[index] != invalid)
            {
                throw duplicate_component {};
            }

            const auto dense_index = dense.size();
            dense.push_back(ent);

            try
            {
                if constexpr (tag_type<Component>)
                {
                    static_assert(sizeof...(Arguments) == 0);
                    components.emplace();
                    sparse[index] = dense_index;
                    return;
                }
                else
                {
                    auto& comp = components.emplace(std::forward<Arguments>(args)...);
                    sparse[index] = dense_index;
                    return comp;
                }
            }
            catch (...)
            {
                dense.pop_back();
                throw;
            }
        }

        /// Returns the mutable component owned by `ent`.
        /// @throws no_such_component if no matching component exists.
        [[nodiscard]]
        Component& get(entity ent)
            requires(!tag_type<Component>)
        {
            return const_cast<Component&>(std::as_const(*this).get(ent));
        }

        /// Returns the component owned by `ent`.
        /// @throws no_such_component if no matching component exists.
        [[nodiscard]]
        const Component& get(entity ent) const
            requires(!tag_type<Component>)
        {
            const auto index = static_cast<size_type>(details::entity_index<Traits>(ent));

            if (index >= sparse.size())
            {
                throw no_such_component {};
            }

            const auto dense_index = sparse[index];

            if (dense_index == invalid || dense_index >= dense.size() || dense[dense_index] != ent)
            {
                throw no_such_component {};
            }

            return components[dense_index];
        }

        /// Erases the component owned by `ent`.
        /// @return `true` when a component was erased; otherwise `false`.
        [[nodiscard]]
        bool erase(entity ent) override
        {
            if (!contains(ent))
            {
                return false;
            }

            const auto index = static_cast<size_type>(details::entity_index<Traits>(ent));
            const auto dense_index = sparse[index];
            const auto last_dense_index = dense.size() - 1;

            if (dense_index != last_dense_index)
            {
                components.replace_with_last(dense_index);
                const auto moved_entity = dense[last_dense_index];
                dense[dense_index] = moved_entity;
                sparse[static_cast<size_type>(details::entity_index<Traits>(moved_entity))] =
                    dense_index;
            }

            dense.pop_back();
            components.pop_back();
            sparse[index] = invalid;

            return true;
        }

        /// Erases every component while retaining allocated sparse storage.
        void clear() noexcept override
        {
            components.clear();
            dense.clear();
            std::fill(sparse.begin(), sparse.end(), invalid);
        }

        /// Returns whether `ent` owns a component in this storage.
        [[nodiscard]]
        bool contains(entity ent) const noexcept override
        {
            const auto index = static_cast<size_type>(details::entity_index<Traits>(ent));

            if (index >= sparse.size())
            {
                return false;
            }

            const auto dense_index = sparse[index];
            return dense_index != invalid && dense_index < dense.size()
                   && dense[dense_index] == ent;
        }

        [[nodiscard]]
        std::size_t size() const noexcept override
        {
            return dense.size();
        }

        [[nodiscard]]
        std::span<const entity> entities() const noexcept override
        {
            return dense;
        }

    private:
        static constexpr size_type invalid = std::numeric_limits<size_type>::max();
        std::vector<entity> dense;
        std::vector<size_type> sparse;
        [[no_unique_address]]
        payload<Component> components;
    };

} // namespace snapfit
