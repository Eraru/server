#include "Camera.hpp"

#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>

#include <iostream>

using namespace os;
using namespace std;

Camera& Camera::getCamera()
{
	static Camera camera;
	return camera;
}

Camera::~Camera()
{
	if (this->recording) this->stop();
}

void Camera::recordThread(int time)
{
	this_thread::sleep_for(chrono::milliseconds(time));
	this->recording = false;
}

void Camera::record(int time)
{
	if ( ! this->recording)
	{
		string command = "raspivid -o os_video.h264 -t " + to_string(time) + " &";
		#ifndef RASPIVID
			command = "";
		#endif
		system(command.c_str());
		this->recording = true;

		if (time > 0)
		{
			thread t(&Camera::recordThread, this, time);
			t.detach();
		}
	}
}

void Camera::record()
{
	this->record(0);
}

void Camera::stop()
{
	system("pkill raspivid");
	this->recording = false;
}