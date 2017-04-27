/****************************************************************************
 *
 * Copyright (C) 2017  Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "mainloop.h"

#include <signal.h>
#include <stdio.h>
#include <poll.h>

#include <linux/videodev2.h>
#include <opencv2/opencv.hpp>

#include "log.h"
#include "util.h"

#define DEFAULT_PIXEL_FORMAT V4L2_PIX_FMT_YUV420

using namespace cv;

static bool _should_run;

static void exit_signal_handler(UNUSED int signum)
{
    _should_run = false;
}

void Mainloop::signal_handlers_setup(void)
{
    struct sigaction sa = { };

    sa.sa_flags = SA_NOCLDSTOP;
    sa.sa_handler = exit_signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

void Mainloop::loop()
{
	Pollable *pollables[] = { _camera, _bmi, _mavlink };
	const uint8_t len = sizeof(pollables) / sizeof(Pollable *);
	struct pollfd desc[len];

	signal_handlers_setup();
	_should_run = true;

	for (uint8_t i = 0; i < len; i++) {
		desc[i].fd = pollables[i]->_fd;
		desc[i].events = POLLIN;
		desc[i].revents = 0;
	}

	while (_should_run) {
		int ret = poll(desc, sizeof(desc) / sizeof(struct pollfd), -1);
		if (ret < 1) {
			continue;
		}

		for (unsigned i = 0; ret && i < (sizeof(desc) / sizeof(struct pollfd)); i++, ret--) {
			for (uint8_t j = 0; j < len; j++) {
				if (desc[i].fd == pollables[j]->_fd) {
					if (desc[i].revents & (POLLIN | POLLPRI)) {
						pollables[j]->handle_read();
					}
					if (desc[i].revents & POLLOUT) {
						pollables[j]->handle_canwrite();
					}
					break;
				}
			}
		}
	}
}

static void _camera_callback(const void *img, size_t len, const struct timeval *timestamp, void *data)
{
	Mainloop *mainloop = (Mainloop *)data;
	mainloop->camera_callback(img, len, timestamp);
}

void Mainloop::camera_callback(const void *img, size_t len, const struct timeval *timestamp)
{
	int dt_us = 0;
	float flow_x_ang = 0, flow_y_ang = 0;

	Mat frame_gray = Mat(_camera->height, _camera->width, CV_8UC1);
	frame_gray.data = (uchar*)img;

	// crop the image (optical flow assumes narrow field of view)
	cv::Rect crop(_camera->width / 2 - _optical_flow->getImageWidth() / 2,
			_camera->height / 2 - _optical_flow->getImageHeight() / 2,
			_optical_flow->getImageWidth(), _optical_flow->getImageHeight());
	cv::Mat cropped_image = frame_gray(crop);

#if DEBUG_LEVEL
	imshow(_window_name, frame_gray);
#endif

	uint32_t img_time_us = timestamp->tv_usec + timestamp->tv_sec * USEC_PER_SEC;
#if DEBUG_LEVEL
	float fps = 0;
#endif

	if (_camera_initial_timestamp) {
		img_time_us -= _camera_initial_timestamp;
#if DEBUG_LEVEL
		fps = 1.0f / ((float)(img_time_us - _camera_prev_timestamp) / USEC_PER_SEC);
#endif
	} else {
		_camera_initial_timestamp = img_time_us;
		img_time_us = 0;
	}

	int flow_quality = _optical_flow->calcFlow(cropped_image.data, img_time_us, dt_us, flow_x_ang, flow_y_ang);

#if DEBUG_LEVEL
	DEBUG("Optical flow quality=%i x=%f y=%f timestamp sec=%lu usec=%lu fps=%f", flow_quality, flow_x_ang, flow_y_ang,
		img_time_us / USEC_PER_SEC, img_time_us % USEC_PER_SEC, fps);
#endif

	_camera_prev_timestamp = img_time_us;

	Point3_<double> gyro_data;
	struct timespec gyro_timespec;
	_bmi->gyro_integrated_get(&gyro_data, &gyro_timespec);

	// check liveness of BMI160
	if (_gyro_last_timespec.tv_sec == gyro_timespec.tv_sec
			&& _gyro_last_timespec.tv_nsec == gyro_timespec.tv_nsec) {
		DEBUG("No new gyroscope data available, sensor is calibrating?");
		return;
	}
	_gyro_last_timespec = gyro_timespec;

#if DEBUG_LEVEL
	DEBUG("Gyro data(%f %f %f)", gyro_data.x, gyro_data.y, gyro_data.z);
#endif

	// check if flow is ready/integrated -> flow output rate
	if (flow_quality < 0) {
		return;
	}

	mavlink_optical_flow_rad_t msg;
	msg.time_usec = timestamp->tv_usec + timestamp->tv_sec * USEC_PER_SEC;
	msg.integration_time_us = dt_us;
	msg.integrated_x = flow_x_ang;
	msg.integrated_y = flow_y_ang;
	msg.integrated_xgyro = gyro_data.x;
	msg.integrated_ygyro = gyro_data.y;
	msg.integrated_zgyro = gyro_data.z;
	msg.time_delta_distance_us = 0;
	msg.distance = -1.0;
	msg.temperature = 0;
	msg.sensor_id = 0;
	msg.quality = flow_quality;

	_mavlink->optical_flow_rad_msg_write(&msg);
}

int Mainloop::run(const char *camera_device, int camera_id,
		uint32_t camera_width, uint32_t camera_height, uint32_t crop_width,
		uint32_t crop_height, unsigned long mavlink_udp_port,
		int flow_output_rate, float focal_length_x, float focal_length_y,
		bool calibrate_bmi, const char *parameters_folder)
{
	int ret;

	_camera = new Camera(camera_device);
	if (!_camera) {
		ERROR("No memory to instantiate Camera");
		return -1;
	}
	ret = _camera->init(camera_id, camera_width, camera_height,
			DEFAULT_PIXEL_FORMAT);
	if (ret) {
		ERROR("Unable to initialize camera");
		goto camera_init_error;
	}

	_mavlink = new Mavlink_UDP();
	if (!_mavlink) {
		ERROR("No memory to instantiate Mavlink_UDP");
		goto mavlink_memory_error;
	}
	ret = _mavlink->init("127.0.0.1", mavlink_udp_port);
	if (ret) {
		ERROR("Unable to initialize mavlink");
		goto mavlink_init_error;
	}

	// TODO: load parameters from yaml file
	_optical_flow = new OpticalFlowOpenCV(focal_length_x, focal_length_y, flow_output_rate, crop_width,
			crop_height);
	if (!_optical_flow) {
		ERROR("No memory to instantiate OpticalFlowOpenCV");
		goto optical_memory_error;
	}
	_camera->callback_set(_camera_callback, this);

	_bmi = new BMI160("/dev/spidev3.0", parameters_folder);
	if (!_bmi) {
		ERROR("No memory to allocate BMI160");
		goto bmi_memory;
	}
	if (_bmi->init()) {
		ERROR("BMI160 init error");
		goto bmi_error;
	}
	if (calibrate_bmi) {
		_bmi->calibrate();
	}
	if (_bmi->start()) {
		ERROR("BMI160 start error");
		goto bmi_error;
	}

#if DEBUG_LEVEL
	namedWindow(_window_name, WINDOW_AUTOSIZE);
	startWindowThread();
#endif

	loop();

#if DEBUG_LEVEL
	destroyAllWindows();
#endif

	_bmi->stop();
	delete _bmi;
	delete _optical_flow;
	delete _mavlink;
	_camera->shutdown();
	delete _camera;

	return 0;

bmi_error:
	delete _bmi;
bmi_memory:
	delete _optical_flow;
optical_memory_error:
mavlink_init_error:
	delete _mavlink;
mavlink_memory_error:
	_camera->shutdown();
camera_init_error:
	delete _camera;
	return -1;
}
