#pragma once

#include <atomic>

/**
 * @brief A fixed clock for reference shots (bridge: communityshaders.world action=refclock).
 *
 * While on, everything of ours that advances with play time reads this fixed number of seconds instead: the cloud
 * field (wind offsets, development phase, storm time, horizon band), the tree sway phase and the shader timer. Two
 * shots of one view taken hours or game starts apart then show the same clouds and the same trees. Off = normal play;
 * the clocks carry on from the reference value.
 */
namespace ReferenceClock
{
	inline std::atomic<bool> on{ false };
	inline std::atomic<float> seconds{ 600.0f };
}
