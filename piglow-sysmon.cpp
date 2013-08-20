/*
	piglow-sysmon
	Copyright 2013 Chris Johnson

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>
#include <fstream>
#include <algorithm>
#include <string>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <errno.h>
#include <wiringPi.h>
#include <sn3218.h>
#include <piGlow.h>
#include <signal.h>
#include <dirent.h>
#include <libgen.h>

using namespace std;

// Utility timer class, for network monitoring
class Timer
{
public:
	Timer()
	{
		(*this)();
	}
	
	double operator()()
	{
		timespec t;
		clock_gettime(CLOCK_REALTIME, &t);
		double retval=(t.tv_sec-seconds)+1e-9*(t.tv_nsec-nanoseconds);
		seconds=t.tv_sec;
		nanoseconds=t.tv_nsec;
		return retval;
	}
	
private:
	time_t seconds;
	long nanoseconds;
};

// Class to monitor temperature, CPU usage and network usage on a Raspberry Pi
class PiMonitor
{
public:
	PiMonitor(const string &netInterface) : netInterface_(netInterface)
	{
		float dummy;
		GetCPUUsage();
		GetNetworkUsage(dummy, dummy);
	}
	float GetCPUUsage()
	{
		ifstream statfile("/proc/stat");
		if (!statfile.is_open()) throw runtime_error("Can't open /proc/stat");
		long user, nice, system, idle, iowait, irq, softirq;
		string header;
		statfile >> header;
		if (header!="cpu") throw runtime_error("Can't find cpu line in /proc/stat");
		statfile >> user >> nice >> system >> idle >> iowait >> irq >> softirq;
		statfile.close();
		long newTotal = user+nice+system+idle+iowait+irq+softirq;
		long newWork = user+nice+system;
		float retval=100.0*static_cast<float>(newWork-workJiffies_)/static_cast<float>(newTotal-totalJiffies_);
		workJiffies_=newWork;
		totalJiffies_=newTotal;
		return retval;
	}
	
	float GetTemperature()
	{
		ifstream tfile("/sys/class/thermal/thermal_zone0/temp");
		if (!tfile.is_open()) throw runtime_error("Can't open /sys/class/thermal/thermal_zone0/temp");
		int tempInMilliCentigrade;
		tfile >> tempInMilliCentigrade;
		tfile.close();
		return tempInMilliCentigrade*0.001;
	}
	
	void GetNetworkUsage(float &receiveBytesPerSecond, float &sendBytesPerSecond)
	{
		unsigned long receiveBytes, sendBytes, dummy;
		double period = networkTimer_();
		ifstream netfile("/proc/net/dev");
		if (!netfile.is_open()) throw runtime_error("Can't open /proc/net/dev");
		while (netfile)
		{ 
			string interface;
			netfile >> interface;
			if (interface==string(netInterface_+":")) break;
			else netfile.ignore(numeric_limits<streamsize>::max(), '\n');

			if (netfile.eof() || netfile.bad())
			{
				ostringstream err;
				err << "Couldn't find network interface " << netInterface_;
				throw runtime_error(err.str().c_str());
			}
		}
		netfile >> receiveBytes >> dummy >> dummy >> dummy >> dummy >> dummy >> dummy >> dummy >> sendBytes;
		netfile.close();
		receiveBytesPerSecond=(receiveBytes-lastReceiveBytes_)/period;
		sendBytesPerSecond=(sendBytes-lastSendBytes_)/period;
		
		lastReceiveBytes_=receiveBytes;
		lastSendBytes_=sendBytes;
	}
private:
	unsigned long totalJiffies_, workJiffies_;
	unsigned long lastSendBytes_, lastReceiveBytes_;
	Timer networkTimer_;
	string netInterface_;
	
};

/// PiGlow bar chart. Value from 0 to 1, brightness from 0 to 100
void PiGlowBar(int leg, float value, int brightness=20)
{
	value=min(max(value,0.0f),1.0f);
	for (int i=0; i<6; i++)
	{
		float intensity=6.0f*(value-static_cast<float>(i)/6.0f);
		intensity=min(max(intensity,0.0f),1.0f);
		piGlow1(leg, i, intensity*brightness);
	}
}

/// I2C module setup -- based on 'gpio' utility in the WiringPi library
/// Requires program to be run as root/sudo
void ChangeOwner(const char *file)
{
	if (chown(file, getuid(), getgid()))
	{
		ostringstream oss;
		if (errno==ENOENT) oss << "File " << file << " not present";
		else oss << "Unable to change ownership of " << file;
		throw runtime_error(oss.str());
	}
}

bool ModuleLoaded(const string &name)
{
	ifstream mods("/proc/modules");
	if (!mods.is_open()) throw runtime_error ("Unable to open /proc/modules");
	string line;
	while (getline(mods,line)) if (line.find(name)!=string::npos) return true;
	return false;
}

void SetupI2C()
{
	bool modulesPreLoaded=true;
	if (!ModuleLoaded("i2c_dev"))
	{
		modulesPreLoaded=false;
		system("modprobe i2c_dev");
	}
	if (!ModuleLoaded("i2c_bcm2708")) 
	{
		modulesPreLoaded=false;
		system("modprobe i2c_bcm2708");
		if (!ModuleLoaded("i2c_bcm2708")) throw runtime_error("Unable to load i2c_bcm2708 module");
	}
	
	if (!modulesPreLoaded)
	{
		sleep(1);
		ChangeOwner("/dev/i2c-0");
		ChangeOwner("/dev/i2c-1");
	}	
}

/// Kill existing instances of the program
void KillExistingInstances(char* name) 
{
	DIR* dir;
	struct dirent* ent;

	// Get the PID and name of the current program
	pid_t localPid = getpid();
	const char *bname = basename(name);

	// Run through /proc/<integer>/stat files
	if (!(dir = opendir("/proc"))) throw runtime_error("Can't open /proc");
	while ((ent = readdir(dir)) != NULL) 
	{
		long lpid = atol(ent->d_name);
		if (lpid <= 0 || lpid==localPid) continue; // skip non-processes and this process

		ostringstream fname;
		fname << "/proc/" << lpid << "/stat";
		ifstream statfile(fname.str().c_str());
		if (!statfile.is_open()) throw runtime_error("Can't open stat file");

		/// Extract process name
		statfile.ignore(numeric_limits<streamsize>::max(), '(');
		string procname;
		getline(statfile, procname, ')');
		statfile.close();
		if (procname.size()==0) throw runtime_error("Can't parse stat file");

		/// Kill existing instances of this program
		if (procname==bname) 
		{
			if (kill(lpid, SIGTERM)!=0)
			{
				ostringstream err;
				err << "Can't kill existing "<< bname << " process (pid "<<lpid<<")";
				throw runtime_error(err.str().c_str());
			}
		}
	}
	closedir(dir);
}

	
static bool quit=false;

void HandleSignals(int signum)
{
	quit=true;
}

void SetupSignals()
{
	struct sigaction newAction, oldAction;
	newAction.sa_handler = HandleSignals;
	sigemptyset(&newAction.sa_mask);
	newAction.sa_flags = 0;

	sigaction(SIGTERM, NULL, &oldAction);
    if (oldAction.sa_handler != SIG_IGN) sigaction(SIGTERM, &newAction, NULL);
	sigaction(SIGINT, NULL, &oldAction);
    if (oldAction.sa_handler != SIG_IGN) sigaction(SIGINT, &newAction, NULL);
	sigaction(SIGHUP, NULL, &oldAction);
    if (oldAction.sa_handler != SIG_IGN) sigaction(SIGHUP, &newAction, NULL);	
}


int main(int argc, char *argv[])
{
	// Default command line arguments
	int opt, ledBrightness=20, consoleMode=0, delayMs=1000;
	string netInterface("eth0");

	// Process command line arguments
	while ((opt = getopt(argc, argv, "c?hb:n:d:")) != -1)
	{
		switch (opt)
		{
	
		case 'b':
			ledBrightness = atoi(optarg);
			if (ledBrightness<1) ledBrightness=1;
			if (ledBrightness>100) ledBrightness=100;
			break;
		case 'd':
			delayMs = atoi(optarg);
			if (delayMs < 10) delayMs=10;
			break;
		case 'n':
			netInterface=string(optarg);
			break;
		case 'c':
			consoleMode=1;
			break;
		case '?': case 'h':
			cout << "Usage: " << argv[0] << " [-b brightness] [-n interface] [-d delay] [-c] [-h] [-?]\n\n" << endl;
			cout << "       -b brightness\n"
			     << "            Sets the maximum LED brightess (between 1 and 100).\n"
			     << "            Default is 20.\n"
			     << "       -n interface\n"
			     << "            Specifies the network interface to monitor.\n"
			     << "            Default is eth0.\n"
			     << "       -d delay\n"
			     << "            Specifies the delay in milliseconds between updates.\n"
			     << "            Minimum is 10. Default is 1000. \n"
			     << "       -c\n"
			     << "            Runs program at the console (when omitted, the default\n"
			     << "            behaviour is to fork a background process)\n"
			     << "       -? -h\n"
			     << "            Show this help screen" << endl;
			return 0;
			break;
		default:
			break;
		}
	}

	try 
	{
		// Setup
		KillExistingInstances(argv[0]);
		PiMonitor pm(netInterface);
		SetupSignals();
		SetupI2C();
		wiringPiSetupSys();
		piGlowSetup(0);
		
		// If not in console mode, fork a background processs
		if (!consoleMode)
		{
			int forkval=fork();
			if (forkval<0)
			{
				cerr << "Fork failed" << endl;
				return 1;
			}
			if (forkval>0) return 0; // In parent process, so quit
			// Must be in child process from here on
		}

		// Main loop
		while (!quit)
		{
			float rec, send, temp, cpu;
			temp=pm.GetTemperature();
			cpu=pm.GetCPUUsage();
			pm.GetNetworkUsage(rec,send);
			
			PiGlowBar(0, (temp-40.0)/40.0, ledBrightness);
			PiGlowBar(1, cpu/100.0, ledBrightness);
			PiGlowBar(2, (log((rec+send)/1e2)/log(10))/6.0, ledBrightness);
			
			usleep(1000*delayMs);
		}
		
		// Turn off all LEDs on quit
		for (int i=0; i<6; i++) for(int l=0; l<3; l++) piGlow1(l, i, 0);
	}
	catch (runtime_error &re)
	{
		cerr << re.what() << endl;
		return 1;
	}	
	return 0;
};
