#include <iostream>
#include <utility>

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

    struct simulated_label;
    struct disabled_label;

    using simulated = snapfit::tag<simulated_label>;
    using disabled = snapfit::tag<disabled_label>;
} // namespace

int main()
{
    using entity = snapfit::entity32;
    using registry = snapfit::registry<snapfit::traits<entity>>;

    registry world;

    const auto player = world.create();
    world.emplace<position>(player, 0.0F, 0.0F);
    world.emplace<velocity>(player, 2.0F, 1.0F);
    world.emplace<simulated>(player);

    const auto npc = world.create();
    world.emplace<position>(npc, 10.0F, 3.0F);
    world.emplace<velocity>(npc, -1.0F, 0.0F);
    world.emplace<simulated>(npc);

    const auto paused = world.create();
    world.emplace<position>(paused, 100.0F, 100.0F);
    world.emplace<velocity>(paused, 50.0F, 50.0F);
    world.emplace<simulated>(paused);
    world.emplace<disabled>(paused);

    constexpr float delta_time = 0.5F;

    // Tags participate in filtering, but do not become elements in the tuple.
    for (auto [ent, pos, vel] :
         world.view<position, velocity>(snapfit::include<simulated>,
                                        snapfit::exclude<disabled>))
    {
        pos.x += vel.x * delta_time;
        pos.y += vel.y * delta_time;

        std::cout << "updated entity " << std::to_underlying(ent) << '\n';
    }

    // A const registry produces const component references.
    const auto& read_only_world = world;
    for (auto [ent, pos] : read_only_world.view<position>())
    {
        std::cout << "entity " << std::to_underlying(ent) << " is at (" << pos.x << ", "
                  << pos.y << ")\n";
    }
}
