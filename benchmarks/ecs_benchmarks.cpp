#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <snapfit/snapfit.h>

namespace
{
    struct position {
        float x;
        float y;
    };

    struct velocity {
        float x;
        float y;
    };

    struct health {
        std::int32_t value;
    };

    template<std::size_t Index>
    struct release_component {
        std::uint32_t value;
    };

    struct selected_label;
    struct disabled_label;
    using selected = snapfit::tag<selected_label>;
    using disabled = snapfit::tag<disabled_label>;

    using entity = snapfit::entity32;
    using registry = snapfit::registry<snapfit::traits<entity>>;

    constexpr std::int64_t small_population = 1 << 10;
    constexpr std::int64_t medium_population = 1 << 16;
    constexpr std::int64_t large_population = 1 << 19;
    constexpr std::int64_t fragmented_population = 1 << 18;

    void set_items_processed(benchmark::State& state, std::size_t population)
    {
        state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(population));
    }

    std::vector<entity> create_positions(registry& world, std::size_t population)
    {
        auto entities = world.create(population);
        for (std::size_t index = 0; index < entities.size(); ++index)
        {
            const auto value = static_cast<float>(index);
            world.emplace<position>(entities[index], value, value);
        }
        return entities;
    }

    void create_velocity(registry& world, const std::vector<entity>& entities, std::size_t stride)
    {
        for (std::size_t index = 0; index < entities.size(); index += stride)
        {
            world.emplace<velocity>(entities[index], 1.0F, -1.0F);
        }
    }

    void create_health(registry& world, const std::vector<entity>& entities, std::size_t stride)
    {
        for (std::size_t index = 0; index < entities.size(); index += stride)
        {
            world.emplace<health>(entities[index], 100);
        }
    }

    void create_filters(registry& world, const std::vector<entity>& entities)
    {
        for (std::size_t index = 0; index < entities.size(); ++index)
        {
            if (index % 2 == 0)
            {
                world.emplace<selected>(entities[index]);
            }
            if (index % 4 == 0)
            {
                world.emplace<disabled>(entities[index]);
            }
        }
    }

    template<std::size_t... Indices>
    void create_release_components(registry& world,
                                   const std::vector<entity>& entities,
                                   std::index_sequence<Indices...>)
    {
        auto create_pool = [&]<std::size_t Index>()
        {
            for (const auto ent : entities)
            {
                world.emplace<release_component<Index>>(ent, static_cast<std::uint32_t>(Index));
            }
        };
        (create_pool.template operator()<Indices>(), ...);
    }

    void create_release_components(registry& world,
                                   const std::vector<entity>& entities,
                                   std::size_t pool_count)
    {
        switch (pool_count)
        {
        case 0:
            break;
        case 1:
            create_release_components(world, entities, std::make_index_sequence<1> {});
            break;
        case 8:
            create_release_components(world, entities, std::make_index_sequence<8> {});
            break;
        case 32:
            create_release_components(world, entities, std::make_index_sequence<32> {});
            break;
        default:
            std::unreachable();
        }
    }

    void create_batch(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        for (auto _ : state)
        {
            registry world;
            auto entities = world.create(population);
            benchmark::DoNotOptimize(entities.data());
        }
        set_items_processed(state, population);
    }

    void release_entities(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        const auto pool_count = static_cast<std::size_t>(state.range(1));
        registry world;
        auto entities = world.create(population);
        create_release_components(world, entities, pool_count);

        for (auto _ : state)
        {
            for (const auto ent : entities)
            {
                world.release(ent);
            }

            state.PauseTiming();
            entities = world.create(population);
            create_release_components(world, entities, pool_count);
            state.ResumeTiming();
        }
        state.counters["storage_pools"] = static_cast<double>(pool_count);
        set_items_processed(state, population);
    }

    void bulk_release_entities(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        const auto pool_count = static_cast<std::size_t>(state.range(1));
        registry world;
        auto entities = world.create(population);
        create_release_components(world, entities, pool_count);

        for (auto _ : state)
        {
            world.release(entities.begin(), entities.end());

            state.PauseTiming();
            entities = world.create(population);
            create_release_components(world, entities, pool_count);
            state.ResumeTiming();
        }
        state.counters["storage_pools"] = static_cast<double>(pool_count);
        set_items_processed(state, population);
    }

    void emplace_components(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        registry world;
        auto entities = world.create(population);

        for (auto _ : state)
        {
            for (const auto ent : entities)
            {
                world.emplace<position>(ent, 1.0F, 2.0F);
            }

            state.PauseTiming();
            for (const auto ent : entities)
            {
                benchmark::DoNotOptimize(world.remove<position>(ent));
            }
            state.ResumeTiming();
        }
        set_items_processed(state, population);
    }

    void remove_components(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        registry world;
        auto entities = create_positions(world, population);

        for (auto _ : state)
        {
            for (const auto ent : entities)
            {
                benchmark::DoNotOptimize(world.remove<position>(ent));
            }

            state.PauseTiming();
            for (const auto ent : entities)
            {
                world.emplace<position>(ent, 1.0F, 2.0F);
            }
            state.ResumeTiming();
        }
        set_items_processed(state, population);
    }

    void component_churn(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        registry world;
        const auto entities = world.create(population);

        for (auto _ : state)
        {
            for (const auto ent : entities)
            {
                world.emplace<position>(ent, 1.0F, 2.0F);
            }
            for (const auto ent : entities)
            {
                benchmark::DoNotOptimize(world.remove<position>(ent));
            }
        }
        set_items_processed(state, population * 2);
    }

    void sequential_get(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        registry world;
        const auto entities = create_positions(world, population);

        for (auto _ : state)
        {
            float sum = 0.0F;
            for (const auto ent : entities)
            {
                sum += world.get<position>(ent).x;
            }
            benchmark::DoNotOptimize(sum);
        }
        set_items_processed(state, population);
    }

    void random_contains(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        registry world;
        auto entities = create_positions(world, population);
        std::mt19937 random { 0x5A17F17U };
        std::ranges::shuffle(entities, random);

        for (auto _ : state)
        {
            std::size_t matches = 0;
            for (const auto ent : entities)
            {
                matches += world.contains<position>(ent) ? 1U : 0U;
            }
            benchmark::DoNotOptimize(matches);
        }
        set_items_processed(state, population);
    }

    void random_get(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        registry world;
        auto entities = create_positions(world, population);
        std::mt19937 random { 0x5A17F17U };
        std::ranges::shuffle(entities, random);

        for (auto _ : state)
        {
            float sum = 0.0F;
            for (const auto ent : entities)
            {
                sum += world.get<position>(ent).x;
            }
            benchmark::DoNotOptimize(sum);
        }
        set_items_processed(state, population);
    }

    void random_contains_and_get(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        registry world;
        auto entities = create_positions(world, population);
        std::mt19937 random { 0x5A17F17U };
        std::ranges::shuffle(entities, random);

        for (auto _ : state)
        {
            float sum = 0.0F;
            for (const auto ent : entities)
            {
                if (world.contains<position>(ent))
                {
                    sum += world.get<position>(ent).x;
                }
            }
            benchmark::DoNotOptimize(sum);
        }
        set_items_processed(state, population);
    }

    void fragmented_random_get(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        registry world;
        auto all_entities = create_positions(world, population * 2);
        std::vector<entity> entities;
        entities.reserve(population);

        for (std::size_t index = 0; index < all_entities.size(); ++index)
        {
            if (index % 2 == 0)
            {
                entities.push_back(all_entities[index]);
            }
            else
            {
                world.release(all_entities[index]);
            }
        }

        std::mt19937 random { 0x5A17F17U };
        std::ranges::shuffle(entities, random);

        for (auto _ : state)
        {
            float sum = 0.0F;
            for (const auto ent : entities)
            {
                sum += world.get<position>(ent).x;
            }
            benchmark::DoNotOptimize(sum);
        }
        set_items_processed(state, population);
    }

    void view_one_required(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        registry world;
        create_positions(world, population);

        for (auto _ : state)
        {
            float sum = 0.0F;
            for (auto [ent, pos] : world.view<position>())
            {
                benchmark::DoNotOptimize(ent);
                sum += pos.x;
            }
            benchmark::DoNotOptimize(sum);
        }
        set_items_processed(state, population);
    }

    void view_two_required_half_match(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        registry world;
        const auto entities = create_positions(world, population);
        create_velocity(world, entities, 2);

        for (auto _ : state)
        {
            float sum = 0.0F;
            for (auto [ent, pos, vel] : world.view<position, velocity>())
            {
                benchmark::DoNotOptimize(ent);
                sum += pos.x + vel.x;
            }
            benchmark::DoNotOptimize(sum);
        }
        set_items_processed(state, population / 2);
    }

    void view_three_required_quarter_match(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        registry world;
        const auto entities = create_positions(world, population);
        create_velocity(world, entities, 2);
        create_health(world, entities, 4);

        for (auto _ : state)
        {
            float sum = 0.0F;
            for (auto [ent, pos, vel, hp] : world.view<position, velocity, health>())
            {
                benchmark::DoNotOptimize(ent);
                sum += pos.x + vel.x + static_cast<float>(hp.value);
            }
            benchmark::DoNotOptimize(sum);
        }
        set_items_processed(state, population / 4);
    }

    void view_required_rejection(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        const auto match_percent = static_cast<std::size_t>(state.range(1));
        const auto match_count = population * match_percent / 100;
        const auto miss_count = population - match_count;

        registry world;
        const auto entities = world.create(population + miss_count);
        for (std::size_t index = 0; index < population; ++index)
        {
            const auto value = static_cast<float>(index);
            world.emplace<position>(entities[index], value, value);
        }
        for (std::size_t index = 0; index < match_count; ++index)
        {
            world.emplace<velocity>(entities[index], 1.0F, -1.0F);
        }
        for (std::size_t index = population; index < entities.size(); ++index)
        {
            world.emplace<velocity>(entities[index], 1.0F, -1.0F);
        }

        for (auto _ : state)
        {
            float sum = 0.0F;
            std::size_t matches = 0;
            for (auto [ent, pos, vel] : world.view<position, velocity>())
            {
                benchmark::DoNotOptimize(ent);
                sum += pos.x + vel.x;
                ++matches;
            }
            benchmark::DoNotOptimize(sum);
            benchmark::DoNotOptimize(matches);
        }
        state.counters["match_percent"] = static_cast<double>(match_percent);
        set_items_processed(state, population);
    }

    void view_optional(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        const auto hit_percent = static_cast<std::size_t>(state.range(1));
        registry world;
        const auto entities = create_positions(world, population);

        if (hit_percent == 100)
        {
            create_velocity(world, entities, 1);
        }
        else if (hit_percent == 50)
        {
            create_velocity(world, entities, 2);
        }

        for (auto _ : state)
        {
            float sum = 0.0F;
            for (auto [ent, pos, vel] : world.view<position>(snapfit::optional<velocity>))
            {
                benchmark::DoNotOptimize(ent);
                sum += pos.x + (vel ? vel->x : 0.0F);
            }
            benchmark::DoNotOptimize(sum);
        }
        state.counters["optional_hit_percent"] = static_cast<double>(hit_percent);
        set_items_processed(state, population);
    }

    void view_include_exclude(benchmark::State& state)
    {
        const auto population = static_cast<std::size_t>(state.range(0));
        registry world;
        const auto entities = create_positions(world, population);
        create_filters(world, entities);

        for (auto _ : state)
        {
            float sum = 0.0F;
            for (auto [ent, pos] :
                 world.view<position>(snapfit::include<selected>, snapfit::exclude<disabled>))
            {
                benchmark::DoNotOptimize(ent);
                sum += pos.x;
            }
            benchmark::DoNotOptimize(sum);
        }
        set_items_processed(state, population / 4);
    }

    void construct_filtered_optional_view(benchmark::State& state)
    {
        registry world;
        const auto first = world.create();
        world.emplace<position>(first, 1.0F, 2.0F);
        world.emplace<velocity>(first, 3.0F, 4.0F);
        world.emplace<selected>(first);

        const auto second = world.create();
        world.emplace<health>(second, 100);
        world.emplace<disabled>(second);

        for (auto _ : state)
        {
            auto view = world.view<position, velocity>(
                snapfit::include<selected>, snapfit::exclude<disabled>, snapfit::optional<health>);
            benchmark::DoNotOptimize(view);
        }
        set_items_processed(state, 1);
    }

    void apply_populations(benchmark::internal::Benchmark* benchmark)
    {
        benchmark->Arg(small_population)->Arg(medium_population)->Arg(large_population);
    }

    void apply_mutation_populations(benchmark::internal::Benchmark* benchmark)
    {
        benchmark->Arg(small_population)->Arg(medium_population);
    }

    void apply_release_populations(benchmark::internal::Benchmark* benchmark)
    {
        for (const auto population : { small_population, medium_population })
        {
            for (const auto pool_count : { 0, 1, 8, 32 })
            {
                benchmark->Args({ population, pool_count });
            }
        }
    }

    void apply_optional_populations(benchmark::internal::Benchmark* benchmark)
    {
        for (const auto population : { small_population, medium_population, large_population })
        {
            benchmark->Args({ population, 0 });
            benchmark->Args({ population, 50 });
            benchmark->Args({ population, 100 });
        }
    }

    void apply_rejection_populations(benchmark::internal::Benchmark* benchmark)
    {
        for (const auto population : { small_population, medium_population, large_population })
        {
            benchmark->Args({ population, 10 });
            benchmark->Args({ population, 50 });
            benchmark->Args({ population, 90 });
        }
    }

    void apply_fragmented_populations(benchmark::internal::Benchmark* benchmark)
    {
        benchmark->Arg(small_population)->Arg(medium_population)->Arg(fragmented_population);
    }
} // namespace

BENCHMARK(create_batch)->Apply(apply_populations);
BENCHMARK(release_entities)->Apply(apply_release_populations);
BENCHMARK(bulk_release_entities)->Apply(apply_release_populations);
BENCHMARK(emplace_components)->Apply(apply_mutation_populations);
BENCHMARK(remove_components)->Apply(apply_mutation_populations);
BENCHMARK(component_churn)->Apply(apply_mutation_populations);
BENCHMARK(sequential_get)->Apply(apply_populations);
BENCHMARK(random_contains)->Apply(apply_populations);
BENCHMARK(random_get)->Apply(apply_populations);
BENCHMARK(random_contains_and_get)->Apply(apply_populations);
BENCHMARK(fragmented_random_get)->Apply(apply_fragmented_populations);
BENCHMARK(view_one_required)->Apply(apply_populations);
BENCHMARK(view_two_required_half_match)->Apply(apply_populations);
BENCHMARK(view_three_required_quarter_match)->Apply(apply_populations);
BENCHMARK(view_required_rejection)->Apply(apply_rejection_populations);
BENCHMARK(view_optional)->Apply(apply_optional_populations);
BENCHMARK(view_include_exclude)->Apply(apply_populations);
BENCHMARK(construct_filtered_optional_view);
