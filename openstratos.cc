#include "openstratos.h"

int main(void)
{
	#if DEBUG
		cout << "[OpenStratos] Starting..." << endl;
	#endif

	if ( ! file_exists(STATE_FILE))
	{
		#if DEBUG
			cout << "[OpenStratos] No state file. Starting main logic..." << endl;
		#endif
		main_logic();
	}
	else
	{
		#if DEBUG
			cout << "[OpenStratos] State file found. Starting safe mode..." << endl;
		#endif
		safe_mode();
	}

	return 0;
}

void os::main_logic()
{
	struct timeval timer;
	gettimeofday(&timer, NULL);
	struct tm* now = gmtime(&timer.tv_sec);

	#if DEBUG
		cout << "[OpenStratos] Current time: " << setfill('0') << setw(2) << now->tm_hour << ":" <<
			setfill('0') << setw(2) << now->tm_min << ":" << setfill('0') << setw(2) << now->tm_sec <<
			" UTC of " << setfill('0') << setw(2) << now->tm_mon << "/" <<
			setfill('0') << setw(2) << now->tm_mday << "/" << (now->tm_year+1900) << endl;
	#endif

	check_or_create("data");
	State state = set_state(INITIALIZING);

	check_or_create("data/logs");
	check_or_create("data/logs/main");
	check_or_create("data/logs/camera");
	check_or_create("data/logs/GPS");
	check_or_create("data/logs/GSM");

	#if DEBUG
		cout << "[OpenStratos] Starting logger..." << endl;
	#endif

	Logger logger("data/logs/main/OpenStratos."+ to_string(now->tm_year+1900) +"-"+
		to_string(now->tm_mon) +"-"+ to_string(now->tm_mday) +"."+ to_string(now->tm_hour) +"-"+
		to_string(now->tm_min) +"-"+ to_string(now->tm_sec) +".log", "OpenStratos");

	#if DEBUG
		cout << "[OpenStratos] Logger started." << endl;
	#endif

	initialize(&logger, now);

	logger.log("Starting battery thread...");
	thread battery_thread(&battery_thread_fn, ref(state));
	logger.log("Battery thread started.");

	logger.log("Starting picture thread...");
	thread picture_thread(&picture_thread_fn, ref(state));
	logger.log("Picture thread started.");

	state = set_state(ACQUIRING_FIX);
	logger.log("State changed to "+ state_to_string(state) +".");

	while (state != SHUT_DOWN)
	{
		if (state == ACQUIRING_FIX)
		{
			aquire_fix(&logger);
			state = set_state(FIX_ACQUIRED);
			logger.log("State changed to "+ state_to_string(state) +".");
		}
		else if (state == FIX_ACQUIRED)
		{
			start_recording(&logger);
			send_init_sms(&logger);
			state = set_state(WAITING_LAUNCH);
			logger.log("State changed to "+ state_to_string(state) +".");
		}
		else if (state == WAITING_LAUNCH)
		{
			wait_launch(&logger);
			state = set_state(GOING_UP);
			logger.log("State changed to "+ state_to_string(state) +".");
		}
		else if (state == GOING_UP)
		{
			go_up(&logger);
			logger.log("Balloon burst.");

			state = set_state(GOING_DOWN);
			logger.log("State changed to "+ state_to_string(state) +".");
		}
		else if (state == GOING_DOWN)
		{
			go_down(&logger);
			state = set_state(LANDED);
			logger.log("State changed to "+ state_to_string(state) +".");
		}
		else if (state == LANDED)
		{
			land(&logger);
			state = set_state(SHUT_DOWN);
			logger.log("State changed to "+ state_to_string(state) +".");
		}
		// else
		// TODO reboot
	}

	logger.log("Joining threads...");
	picture_thread.join();
	battery_thread.join();
	logger.log("Threads joined.");

	shut_down(&logger);
}

void os::safe_mode()
{
	// TODO
}

inline bool os::has_launched()
{
	double first_altitude = GPS::get_instance().get_altitude();
	this_thread::sleep_for(5s);
	double second_altitude = GPS::get_instance().get_altitude();

	#if defined SIM || defined REAL_SIM
		return true;
	#else
		return second_altitude > first_altitude + 10;
	#endif
}

inline bool os::has_bursted()
{
	double first_altitude = GPS::get_instance().get_altitude();
	this_thread::sleep_for(6s);
	double second_altitude = GPS::get_instance().get_altitude();

	#if defined SIM || defined REAL_SIM
		return true;
	#else
		return second_altitude < first_altitude - 10;
	#endif
}

inline bool os::has_landed()
{
	double first_altitude = GPS::get_instance().get_altitude();
	this_thread::sleep_for(5s);
	double second_altitude = GPS::get_instance().get_altitude();

	return abs(first_altitude-second_altitude) < 5;
}

void os::initialize(Logger* logger, tm* now)
{
	check_or_create("data/video", logger);
	check_or_create("data/img", logger);

	float available_disk_space = get_available_disk_space();

	logger->log("Available disk space: " + to_string(available_disk_space/1073741824) + " GiB");
	if (available_disk_space < FLIGHT_LENGTH*9437184000) // 1.25 times the flight length
	{
		logger->log("Error: Not enough disk space.");
		exit(1);
	}

	logger->log("Disk space enough for about " + to_string(available_disk_space/7549747200) +
		" hours of fullHD video.");

	logger->log("Initializing WiringPi...");
	wiringPiSetup();
	logger->log("WiringPi initialized.");

	logger->log("Initializing GPS...");
	if ( ! GPS::get_instance().initialize())
	{
		logger->log("GPS initialization error.");
		exit(1);
	}
	logger->log("GPS initialized.");

	logger->log("Initializing GSM...");
	if ( ! GSM::get_instance().initialize())
	{
		logger->log("GSM initialization error.");
		logger->log("Turning GPS off...");
		if (GPS::get_instance().turn_off())
			logger->log("GPS off.");
		else
			logger->log("Error turning GPS off.");

		exit(1);
	}
	logger->log("GSM initialized.");

	logger->log("Checking batteries...");
	double main_battery, gsm_battery;
	GSM::get_instance().get_battery_status(main_battery, gsm_battery);
	logger->log("Batteries checked => Main battery: "+ (main_battery > -1 ? to_string(main_battery*100)+"%" : "disconnected") +
		" - GSM battery: "+ to_string(gsm_battery*100) +"%");

	if ((main_battery < 0.95  && main_battery > -1) || gsm_battery < 0.95)
	{
		logger->log("Error: Not enough battery.");

		logger->log("Turning GSM off...");
		if (GSM::get_instance().turn_off())
			logger->log("GSM off.");
		else
			logger->log("Error turning GSM off.");

		logger->log("Turning GPS off...");
		if (GPS::get_instance().turn_off())
			logger->log("GPS off.");
		else
			logger->log("Error turning GPS off.");

		exit(1);
	}

	logger->log("Waiting for GSM connectivity...");
	while ( ! GSM::get_instance().has_connectivity())
	{
		this_thread::sleep_for(1s);
	}
	logger->log("GSM connected.");

	logger->log("Testing camera recording...");
	#ifndef RASPICAM
		logger->log("Error: No raspivid found. Is this a Raspberry?");
		exit(1);
	#endif
	logger->log("Recording 10 seconds as test...");
	if ( ! Camera::get_instance().record(10000))
	{
		logger->log("Error starting recording");
		exit(1);
	}
	this_thread::sleep_for(11s);
	if (file_exists("data/video/test.h264"))
	{
		logger->log("Camera test OK.");

		logger->log("Removing test file...");
		if (remove("data/video/test.h264"))
			logger->log("Error removing test file.");
		else
			logger->log("Test file removed.");
	}
	else
	{
		logger->log("Test recording error.");
		logger->log("Turning GSM off...");
		if (GSM::get_instance().turn_off())
			logger->log("GSM off.");
		else
			logger->log("Error turning GSM off.");

		logger->log("Turning GPS off...");
		if (GPS::get_instance().turn_off())
			logger->log("GPS off.");
		else
			logger->log("Error turning GPS off.");

		exit(1);
	}
}

void os::aquire_fix(Logger* logger)
{
	while ( ! GPS::get_instance().is_active())
		this_thread::sleep_for(1s);

	logger->log("GPS fix acquired, waiting for date change.");
	this_thread::sleep_for(2s);

	struct timezone tz = {0, 0};
	tm gps_time = GPS::get_instance().get_time();
	struct timeval tv = {timegm(&gps_time), 0};
	settimeofday(&tv, &tz);

	logger->log("System date change.");
}

void os::start_recording(Logger* logger)
{
	logger->log("Starting video recording...");
	if ( ! Camera::get_instance().record())
	{
		logger->log("Error starting recording");
		logger->log("Turning GSM off...");
		if (GSM::get_instance().turn_off())
			logger->log("GSM off.");
		else
			logger->log("Error turning GSM off.");

		logger->log("Turning GPS off...");
		if (GPS::get_instance().turn_off())
			logger->log("GPS off.");
		else
			logger->log("Error turning GPS off.");

		exit(1);
	}
	logger->log("Recording started.");
}

void os::send_init_sms(Logger* logger)
{
	logger->log("Sending initialization SMS...");
	if ( ! GSM::get_instance().send_SMS("Initialization finished OK. Recording. Waiting for launch.", SMS_PHONE))
	{
		logger->log("Error sending initialization SMS.");

		logger->log("Stoping video recording.");
		if (Camera::get_instance().stop())
		{
			logger->log("Recording stopped.");
		}
		else
		{
			logger->log("Error stopping recording.");
		}

		logger->log("Turning GSM off...");
		if (GSM::get_instance().turn_off())
			logger->log("GSM off.");
		else
			logger->log("Error turning GSM off.");

		logger->log("Turning GPS off...");
		if (GPS::get_instance().turn_off())
			logger->log("GPS off.");
		else
			logger->log("Error turning GPS off.");

		exit(1);
	}
	logger->log("Initialization SMS sent.");
}

void os::wait_launch(Logger* logger)
{
	logger->log("Waiting for launch...");
	#ifdef SIM
		this_thread::sleep_for(2min);
	#endif
	#ifdef REAL_SIM
		this_thread::sleep_for(20min);
	#endif

	while ( ! has_launched())
		this_thread::sleep_for(1s);

	logger->log("Balloon launched.");
}

void os::go_up(Logger* logger)
{
	logger->log("Trying to send launch confirmation SMS...");
	if ( ! GSM::get_instance().send_SMS("Launched in Lat: "+
		to_string(GPS::get_instance().get_latitude()) +" and Lon: "+
		to_string(GPS::get_instance().get_longitude()) +".", SMS_PHONE))
	{
		logger->log("Error sending launch confirmation SMS.");
	}
	else
	{
		logger->log("Launch confirmation SMS sent.");
	}

	#if !defined SIM && !defined REAL_SIM
		while (GPS::get_instance().get_altitude() < 1500)
		{
			this_thread::sleep_for(2s);
		}
	#else
		this_thread::sleep_for(275s);
	#endif
	logger->log("1.5 km mark.");
	logger->log("Trying to send \"going up\" SMS...");
	if ( ! GSM::get_instance().send_SMS("1.5 km mark passed going up in Lat: "+
	to_string(GPS::get_instance().get_latitude()) +" and Lon: "+
	to_string(GPS::get_instance().get_longitude()) +".", SMS_PHONE))
	{
		logger->log("Error sending \"going up\" SMS.");
	}
	else
	{
		logger->log("\"Going up\" SMS sent.");
	}

	logger->log("Turning off GSM...");
	GSM::get_instance().turn_off();
	logger->log("GSM off.");

	bool bursted = false;

	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(2min);
		logger->log("5 km mark passed going up.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(1435s);
		logger->log("5 km mark passed going up.");
	#else
		while ( ! (bursted = has_bursted()) && GPS::get_instance().get_altitude() < 5000);
		if ( ! bursted) logger->log("5 km mark passed going up.");
		else return;
	#endif

	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(2min);
		logger->log("10 km mark passed going up.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(2000s);
		logger->log("10 km mark passed going up.");
	#else
		while ( ! (bursted = has_bursted()) && GPS::get_instance().get_altitude() < 10000);
		if ( ! bursted) logger->log("10 km mark passed going up.");
		else return;
	#endif

	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(2min);
		logger->log("15 km mark passed going up.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(2000s);
		logger->log("15 km mark passed going up.");
	#else
		while ( ! (bursted = has_bursted()) && GPS::get_instance().get_altitude() < 15000);
		if ( ! bursted) logger->log("15 km mark passed going up.");
		else return;
	#endif

	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(2min);
		logger->log("20 km mark passed going up.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(2000s);
		logger->log("20 km mark passed going up.");
	#else
		while ( ! (bursted = has_bursted()) && GPS::get_instance().get_altitude() < 20000);
		if ( ! bursted) logger->log("20 km mark passed going up.");
		else return;
	#endif

	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(2min);
		logger->log("25 km mark passed going up.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(2000s);
		logger->log("25 km mark passed going up.");
	#else
		while ( ! (bursted = has_bursted()) && GPS::get_instance().get_altitude() < 25000);
		if ( ! bursted) logger->log("25 km mark passed going up.");
		else return;
	#endif

	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(2min);
		logger->log("30 km mark passed going up.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(2000s);
		logger->log("30 km mark passed going up.");
	#else
		while ( ! (bursted = has_bursted()) && GPS::get_instance().get_altitude() < 30000);
		if ( ! bursted) logger->log("30 km mark passed going up.");
		else return;
	#endif

	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(2min);
		logger->log("35 km mark passed going up.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(2000s);
		logger->log("35 km mark passed going up.");
	#else
		while ( ! (bursted = has_bursted()) && GPS::get_instance().get_altitude() < 35000);
		if ( ! bursted) logger->log("35 km mark passed going up.");
		else return;
	#endif

	while ( ! has_bursted());
}

void os::go_down(Logger* logger)
{
	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(1min);
		logger->log("25 km mark passed going down.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(5min);
		logger->log("25 km mark passed going down.");
	#else
		while (GPS::get_instance().get_altitude() > 25000)
			this_thread::sleep_for(5s);

		logger->log("25 km mark passed going down.");
	#endif

	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(1min);
		logger->log("15 km mark passed going down.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(650s);
		logger->log("15 km mark passed going down.");
	#else
		while (GPS::get_instance().get_altitude() > 15000)
			this_thread::sleep_for(5s);

		logger->log("15 km mark passed going down.");
	#endif

	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(2min);
		logger->log("5 km mark passed going down.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(1400s);
		logger->log("5 km mark passed going down.");
	#else
		while (GPS::get_instance().get_altitude() > 5000)
			this_thread::sleep_for(5s);

		logger->log("5 km mark passed going down.");
	#endif

	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(1min);
		logger->log("2.5 km mark passed going down.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(500s);
		logger->log("2.5 km mark passed going down.");
	#else
		while (GPS::get_instance().get_altitude() > 2500)
			this_thread::sleep_for(5s);

		logger->log("2.5 km mark passed going down.");
	#endif

	logger->log("Turning on GSM...");
	GSM::get_instance().turn_on();

	logger->log("Waiting for GSM connectivity...");
	int count = 0;
	while ( ! GSM::get_instance().has_connectivity())
	{
		if (count == 20) break;
		this_thread::sleep_for(1s);
		++count;
	}
	if (count == 20)
	{
		logger->log("No connectivity, waiting for 1.5 km mark or landing.");
	}
	else
	{
		logger->log("GSM connected.");

		logger->log("Trying to send first SMS...");
		if ( ! GSM::get_instance().send_SMS("2.5 km mark passed going down in Lat: "+ to_string(GPS::get_instance().get_latitude())
			+" and Lon: "+ to_string(GPS::get_instance().get_longitude()) +".", SMS_PHONE))
		{
			logger->log("Error sending first SMS.");
		}
		else
		{
			logger->log("First SMS sent.");
		}
	}

	bool landed = false;

	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(1min);
		logger->log("1.5 km mark passed going down.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(175s);
		logger->log("1.5 km mark passed going down.");
	#else
		while (GPS::get_instance().get_altitude() > 1500 && ! (landed = has_landed()));
		if ( ! landed) logger->log("1.5 km mark passed going down.");
	#endif

	if ( ! landed)
	{
		count = 0;
		while ( ! GSM::get_instance().has_connectivity())
		{
			if (count == 20) break;
			this_thread::sleep_for(1s);
			++count;
		}
		if (count == 20)
		{
			logger->log("No connectivity, waiting for 500 m mark or landing.");
		}
		else
		{
			logger->log("GSM connected.");

			logger->log("Trying to send second SMS...");
			if ( ! GSM::get_instance().send_SMS("1.5 km mark passed going down in Lat: "+ to_string(GPS::get_instance().get_latitude())
				+" and Lon: "+ to_string(GPS::get_instance().get_longitude()) +".", SMS_PHONE))
			{
				logger->log("Error sending second SMS.");
			}
			else
			{
				logger->log("Second SMS sent.");
			}
		}
	}

	#if defined SIM && !defined REAL_SIM
		this_thread::sleep_for(1min);
		logger->log("500 m mark passed going down.");
	#elif defined REAL_SIM && !defined SIM
		this_thread::sleep_for(225s);
		logger->log("500 m mark passed going down.");
	#else
		while (GPS::get_instance().get_altitude() > 500 && ! (landed = has_landed()));
		if ( ! landed) logger->log("500 m mark passed going down.");
	#endif

	if ( ! landed)
	{
		logger->log("500 m mark.");

		count = 0;
		while ( ! GSM::get_instance().has_connectivity())
		{
			if (count > 15) break;
			this_thread::sleep_for(1s);
			++count;
		}
		if ( ! GSM::get_instance().has_connectivity())
		{
			logger->log("No connectivity, waiting for landing.");
		}
		else
		{
			logger->log("GSM connected.");

			logger->log("Trying to send third SMS...");
			if ( ! GSM::get_instance().send_SMS("500 m mark passed in Lat: "+ to_string(GPS::get_instance().get_latitude())
				+" and Lon: "+ to_string(GPS::get_instance().get_longitude()) +".", SMS_PHONE))
			{
				logger->log("Error sending third SMS.");
			}
			else
			{
				logger->log("Third SMS sent.");
			}
		}
	}

	while ( ! has_landed());
	logger->log("Landed.");
}

void os::land(Logger* logger)
{
	logger->log("Stopping video...");
	if ( ! Camera::get_instance().stop())
		logger->log("Error stopping video.");
	else
		logger->log("Video stopped.");

	logger->log("Waiting 1 minute before sending landed SMS...");
	this_thread::sleep_for(1min);

	logger->log("Sending landed SMS...");
	if ( ! GSM::get_instance().send_SMS("Landed in Lat: "+ to_string(GPS::get_instance().get_latitude())
		+" and Lon: "+ to_string(GPS::get_instance().get_longitude()) +".", SMS_PHONE))
	{
		logger->log("Error sending landed SMS. Trying again in 10 minutes...");
	}
	else
	{
		logger->log("Landed SMS sent. Sending backup SMS in 10 minutes...");
	}

	this_thread::sleep_for(10min);

	logger->log("Sending second landed SMS...");

	double main_battery = 1, gsm_battery = 1;
	while ( ! GSM::get_instance().send_SMS("Landed in Lat: "+ to_string(GPS::get_instance().get_latitude())
		+" and Lon: "+ to_string(GPS::get_instance().get_longitude()) +".", SMS_PHONE) &&
		(main_battery >= 0.05 || main_battery < -1) && gsm_battery >= 0.05)
	{
		logger->log("Error sending second SMS, trying again in 5 minutes.");
		this_thread::sleep_for(5min);
		GSM::get_instance().get_battery_status(main_battery, gsm_battery);
	}

	if ((main_battery < 0.05 && main_battery > -1) || gsm_battery < 0.05)
	{
		logger->log("Not enough battery.");
		logger->log("Main battery: "+ to_string(main_battery) +
			"% - GSM battery: "+ to_string(gsm_battery) +"%");
	}
	else
	{
		logger->log("Second SMS sent.");
	}
}

void os::shut_down(Logger* logger)
{
	logger->log("Shutting down...");

	logger->log("Turning GSM off...");
	if (GSM::get_instance().turn_off())
		logger->log("GSM off.");
	else
		logger->log("Error turning GSM off.");

	logger->log("Turning GPS off...");
	if (GPS::get_instance().turn_off())
		logger->log("GPS off.");
	else
		logger->log("Error turning GPS off.");

	logger->log("Powering off...");
	sync();
	// reboot(RB_POWER_OFF);
}

void os::picture_thread_fn(State& state)
{
	struct timeval timer;
	gettimeofday(&timer, NULL);
	struct tm * now = gmtime(&timer.tv_sec);

	Logger logger("data/logs/camera/Pictures."+ to_string(now->tm_year+1900) +"-"+ to_string(now->tm_mon) +"-"+
		to_string(now->tm_mday) +"."+ to_string(now->tm_hour) +"-"+ to_string(now->tm_min) +"-"+
		to_string(now->tm_sec) +".log", "Pictures");

	logger.log("Waiting for launch...");

	while (state != GOING_UP)
	{
		this_thread::sleep_for(10s);
	}

	logger.log("Launched, waiting 2 minutes for first picture...");
	this_thread::sleep_for(2min);

	while (state == GOING_UP)
	{
		logger.log("Taking picture...");

		if ( ! Camera::get_instance().take_picture(generate_exif_data()))
		{
			logger.log("Error taking picture. Trying again in 30 seconds...");
		}
		else
		{
			logger.log("Picture taken correctly. Next picture in 30 seconds...");
		}

		this_thread::sleep_for(30s);
		logger.log("Taking picture...");

		if ( ! Camera::get_instance().take_picture(generate_exif_data()))
		{
			logger.log("Error taking picture. Next picture in 4 minutes...");
		}
		else
		{
			logger.log("Picture taken correctly. Next picture in 4 minutes...");
		}

		this_thread::sleep_for(4min);
	}

	logger.log("Going down, no more pictures are being taken, picture thread is closing.");
}

void os::battery_thread_fn(State& state)
{
	struct timeval timer;
	gettimeofday(&timer, NULL);
	struct tm * now = gmtime(&timer.tv_sec);

	Logger logger("data/logs/GSM/Battery."+ to_string(now->tm_year+1900) +"-"+ to_string(now->tm_mon) +"-"+
		to_string(now->tm_mday) +"."+ to_string(now->tm_hour) +"-"+ to_string(now->tm_min) +"-"+
		to_string(now->tm_sec) +".log", "Battery");

	double main_battery, gsm_battery;

	while (state != SHUT_DOWN)
	{
		if (GSM::get_instance().get_status())
		{
			GSM::get_instance().get_battery_status(main_battery, gsm_battery);
			logger.log("Main: "+ to_string(main_battery));
			logger.log("GSM: "+ to_string(gsm_battery));
		}

		this_thread::sleep_for(3min);
	}
}

void os::check_or_create(const string& path, Logger* logger)
{
	if ( ! file_exists(path))
	{
		if (logger != NULL)
			logger->log("No '"+path+"' directory, creating...");
		#if DEBUG
			else
				cout << "[OpenStratos] No '"+path+"' directory, creating..." << endl;
		#endif
		if (mkdir(path.c_str(), 0755) != 0)
		{
			if (logger != NULL)
				logger->log("Error creating '"+path+"' directory.");
			#if DEBUG
				else
					cout << "[OpenStratos] Error creating '"+path+"' directory." << endl;
			#endif
			exit(1);

		}
		else
		{
			if (logger != NULL)
				logger->log("'"+path+"' directory created.");
			#if DEBUG
				else
					cout << "[OpenStratos] '"+path+"' directory created." << endl;
			#endif
		}
	}
}

inline bool os::file_exists(const string& name)
{
	struct stat buffer;
	return stat(name.c_str(), &buffer) == 0;
}

inline float os::get_available_disk_space()
{
	struct statvfs fs;
	statvfs("data", &fs);

	return ((float) fs.f_bsize)*fs.f_bavail;
}

const string os::generate_exif_data()
{
	string exif;
	while (GPS::get_instance().get_PDOP() > 5)
	{
		this_thread::sleep_for(1s);
	}
	double gps_lat = GPS::get_instance().get_latitude();
	double gps_lon = GPS::get_instance().get_longitude();
	double gps_alt = GPS::get_instance().get_altitude();
	uint_fast8_t gps_sat = GPS::get_instance().get_satellites();
	float gps_pdop = GPS::get_instance().get_PDOP();
	euc_vec gps_velocity = GPS::get_instance().get_velocity();

	exif += "GPSLatitudeRef="+to_string(gps_lat > 0 ? 'N' : 'S');
	exif += " GPSLatitude="+to_string(abs((int) gps_lat*1000000))+"/1000000,0/1,0/1";
	exif += " GPSLongitudeRef="+to_string(gps_lon > 0 ? 'E' : 'W');
	exif += " GPSLongitude="+to_string(abs((int) gps_lon*1000000))+"/1000000,0/1,0/1";
	exif += " GPSAltitudeRef=0 GPSAltitude="+to_string(gps_alt);
	exif += " GPSSatellites="+to_string(gps_sat);
	exif += " GPSDOP="+to_string(gps_pdop);
	exif += " GPSSpeedRef=K GPSSpeed="+to_string(gps_velocity.speed*3.6);
	exif += " GPSTrackRef=T GPSTrack="+to_string(gps_velocity.course);
	exif += " GPSDifferential=0";
}

State os::set_state(State new_state)
{
	ofstream state_file(STATE_FILE);
	state_file << state_to_string(new_state);
	state_file.close();

	return new_state;
}

State os::get_last_state()
{
	ifstream state_file(STATE_FILE);
	string str_state((istreambuf_iterator<char>(state_file)),
                 istreambuf_iterator<char>());
	state_file.close();

	if (str_state == "INITIALIZING") return INITIALIZING;
	if (str_state == "ACQUIRING_FIX") return ACQUIRING_FIX;
	if (str_state == "FIX_ACQUIRED") return FIX_ACQUIRED;
	if (str_state == "WAITING_LAUNCH") return WAITING_LAUNCH;
	if (str_state == "GOING_UP") return GOING_UP;
	if (str_state == "GOING_DOWN") return GOING_DOWN;
	if (str_state == "LANDED") return LANDED;
	if (str_state == "SHUT_DOWN") return SHUT_DOWN;
	if (str_state == "SAFE_MODE") return SAFE_MODE;

	return RECOVERY;
}

const string os::state_to_string(State state)
{
	switch (state)
	{
		case INITIALIZING:
			return "INITIALIZING";
		break;
		case ACQUIRING_FIX:
			return "ACQUIRING_FIX";
		break;
		case FIX_ACQUIRED:
			return "FIX_ACQUIRED";
		break;
		case WAITING_LAUNCH:
			return "WAITING_LAUNCH";
		break;
		case GOING_UP:
			return "GOING_UP";
		break;
		case GOING_DOWN:
			return "GOING_DOWN";
		break;
		case LANDED:
			return "LANDED";
		break;
		case SHUT_DOWN:
			return "SHUT_DOWN";
		break;
		case SAFE_MODE:
			return "SAFE_MODE";
		break;
		case RECOVERY:
			return "RECOVERY";
	}
}
