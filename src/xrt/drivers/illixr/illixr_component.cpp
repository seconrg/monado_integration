extern "C" {
#include "xrt/xrt_device.h"
}

#include "common/data_format.hpp"
#include "common/phonebook.hpp"
#include "common/plugin.hpp"
#include "common/pose_prediction.hpp"
#include "common/relative_clock.hpp"
#include "common/switchboard.hpp"

#include <array>
#include <iostream>

using namespace ILLIXR;

/// Interface to switchboard and the rest of the runtime
class illixr_plugin : public plugin {
public:
	illixr_plugin(std::string name_, phonebook* pb_)
		: plugin{name_, pb_}
		, sb{pb->lookup_impl<switchboard>()}
		, sb_pose{pb->lookup_impl<pose_prediction>()}
		, _m_clock{pb->lookup_impl<RelativeClock>()}
		, sb_eyebuffer{sb->get_writer<rendered_frame>("eyebuffer")}
		, sb_vsync_estimate{sb->get_reader<switchboard::event_wrapper<time_point>>("vsync_estimate")}
		, _m_slow_pose {sb->get_reader<pose_type>("slow_pose")}
	{ 
		_last_slow_pose_tick = -1;
	}

	const std::shared_ptr<switchboard> sb;
	const std::shared_ptr<pose_prediction> sb_pose;
	std::shared_ptr<RelativeClock> _m_clock;
	switchboard::writer<rendered_frame> sb_eyebuffer;
	switchboard::reader<switchboard::event_wrapper<time_point>> sb_vsync_estimate;
	switchboard::reader<pose_type> _m_slow_pose;
	long _last_slow_pose_tick; // record the time when the last tick 
	fast_pose_type prev_pose; /* stores a copy of pose each time illixr_read_pose() is called */
	time_point sample_time; /* when prev_pose was stored */
};

static illixr_plugin* illixr_plugin_obj = nullptr;

extern "C" plugin* illixr_monado_create_plugin(phonebook* pb) {
	illixr_plugin_obj = new illixr_plugin {"illixr_plugin", pb};
	illixr_plugin_obj->start();
	return illixr_plugin_obj;
}

extern "C" struct xrt_pose illixr_read_pose() {
	assert(illixr_plugin_obj && "illixr_plugin_obj must be initialized first.");

	if (!illixr_plugin_obj->sb_pose->fast_pose_reliable()) {
		std::cerr << "Pose not reliable yet; returning best guess" << std::endl;
	}
	struct xrt_pose ret;
	const fast_pose_type fast_pose = illixr_plugin_obj->sb_pose->get_fast_pose();
	
	const pose_type pose = fast_pose.pose;

	// record when the pose was read for use in write_frame
	illixr_plugin_obj->sample_time = illixr_plugin_obj->_m_clock->now();

	ret.orientation.x = pose.orientation.x();
	ret.orientation.y = pose.orientation.y();
	ret.orientation.z = pose.orientation.z();
	ret.orientation.w = pose.orientation.w();
	ret.position.x = pose.position.x();
	ret.position.y = pose.position.y();
	ret.position.z = pose.position.z();
	// add the position type
	switchboard::ptr<const pose_type> slow_pose = illixr_plugin_obj->_m_slow_pose.get_ro_nullable();
	if (slow_pose){
		long slow_pose_tick = slow_pose->sensor_time.time_since_epoch().count();
		// printf("Slow pose sensor time: %ld", slow_pose_tick);
		if (slow_pose_tick > illixr_plugin_obj->_last_slow_pose_tick){
			illixr_plugin_obj->_last_slow_pose_tick = slow_pose_tick;
			ret.pose_type = keyframe_gen;
		}else{
			ret.pose_type = prediction_gen;
		}
	}else{
		ret.pose_type = prediction_gen;
	}
	
	// store pose in static variable for use in write_frame
	illixr_plugin_obj->prev_pose = fast_pose; // copy member variables

	return ret;
}

extern "C" void illixr_write_frame(unsigned int left, unsigned int right) {
	assert(illixr_plugin_obj != nullptr && "illixr_plugin_obj must be initialized first.");

	static unsigned int buffer_to_use = 0U;

	illixr_plugin_obj->sb_eyebuffer.put(illixr_plugin_obj->sb_eyebuffer.allocate<rendered_frame>(
		rendered_frame {
			std::array<GLuint, 2>{ left, right },
			std::array<GLuint, 2>{ buffer_to_use, buffer_to_use },
			illixr_plugin_obj->prev_pose,
			illixr_plugin_obj->sample_time,
			illixr_plugin_obj->_m_clock->now()
		}
	));

	buffer_to_use = (buffer_to_use == 0U) ? 1U : 0U;
}

extern "C" int64_t illixr_get_vsync_ns() {
	assert(illixr_plugin_obj != nullptr && "illixr_plugin_obj must be initialized first.");

	switchboard::ptr<const switchboard::event_wrapper<time_point>> vsync_estimate =
		illixr_plugin_obj->sb_vsync_estimate.get_ro_nullable();

	time_point target_time = vsync_estimate == nullptr
		? illixr_plugin_obj->_m_clock->now() + display_params::period
		: **vsync_estimate;

	return std::chrono::nanoseconds{target_time.time_since_epoch()}.count();
}

extern "C" int64_t illixr_get_now_ns() {
	assert(illixr_plugin_obj && "illixr_plugin_obj must be initialized first.");
	return std::chrono::duration_cast<std::chrono::nanoseconds>((illixr_plugin_obj->_m_clock->now()).time_since_epoch()).count();
}
