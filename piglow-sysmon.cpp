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


using namespace std;


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

class PiMonitor
{
public:
	PiMonitor()
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
		float retval=100.0*static_cast<float>(newWork-workJiffies)/static_cast<float>(newTotal-totalJiffies);
		workJiffies=newWork;
		totalJiffies=newTotal;
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
		double period = networkTimer();
		ifstream netfile("/proc/net/dev");
		if (!netfile.is_open()) throw runtime_error("Can't open /sys/class/thermal/thermal_zone0/temp");
		while(1)
		{ 
			string interface;
			netfile >> interface;
			if (interface=="eth0:") break;
			else 
			netfile.ignore(numeric_limits<streamsize>::max(), '\n');
		}
		netfile >> receiveBytes >> dummy >> dummy >> dummy >> dummy >> dummy >> dummy >> dummy >> sendBytes;
		netfile.close();
		receiveBytesPerSecond=(receiveBytes-lastReceiveBytes)/period;
		sendBytesPerSecond=(sendBytes-lastSendBytes)/period;
		
		lastReceiveBytes=receiveBytes;
		lastSendBytes=sendBytes;
	}
private:
	unsigned long totalJiffies, workJiffies;
	unsigned long lastSendBytes, lastReceiveBytes;
	Timer networkTimer;
	
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
	if (!ModuleLoaded("i2c_dev")) system("modprobe i2c_dev") ;
	if (!ModuleLoaded("i2c_bcm2708")) system("modprobe i2c_bcm2708") ;

	if (!ModuleLoaded("i2c_bcm2708")) throw runtime_error("Unable to load i2c_bcm2708 module");
	
	sleep(1);
	ChangeOwner("/dev/i2c-0");
	ChangeOwner("/dev/i2c-1");	
	
}

void KillExistingInstances(const char* name) 
{
    DIR* dir;
    struct dirent* ent;
    char buf[512];

    long  pid;
    char pname[100] = {0,};
    char state;
    FILE *fp=NULL; 

    if (!(dir = opendir("/proc"))) {
        perror("can't open /proc");
        return -1;
    }

    while((ent = readdir(dir)) != NULL) {
        long lpid = atol(ent->d_name);
        if (lpid < 0) continue;
        
        ostringstream fname;
        fname << "/proc/" << lpid << "/stat":
        
        ifstream statfile(fname.str());
        if (!statfile.is_open()) throw runtime_error("Can't open stat file");
        
        string procname;
        int pid;
        statfile >> pid >> procname;
        statfile.close();
        
        if (procname.size()<3 || procname[0]!='(' || procname[procname.size()-1]!=')') throw runtime_error("Can't parse stat file");
        procname=procname.substr(1,procname.size()-2); // trim parentheses
        
        if (procname==name) kill(pid, SIGTERM);
        if (fp) {
            if ( (fscanf(fp, "%ld (%[^)]) %c", &pid, pname, &state)) != 3 ){
                printf("fscanf failed \n");
                fclose(fp);
                closedir(dir);
                return -1; 
            }
            if (!strcmp(pname, name)) {
                fclose(fp);
                closedir(dir);
                return (pid_t)lpid;
            }
            fclose(fp);
        }
    }


closedir(dir);
return -1;
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
	int opt, ledBrightness=20, consoleMode=0;
	while ((opt = getopt(argc, argv, "c?hb:")) != -1)
	{
		switch (opt)
		{
	
		case 'b':
			ledBrightness = atoi(optarg);
			break;
		case 'c':
			consoleMode=1;
			break;
		case '?': case 'h':
			cout << "Usage: " << argv[0] << " [-b brightness] [-c] [-h] [-?]\n\n" << endl;
			cout << "       -b brightness\n"
			     << "            sets the maximum LED brightess (between 1 and 100)\n"
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
		PiMonitor pm;
		SetupSignals();
		SetupI2C();
		wiringPiSetupSys();
		piGlowSetup(0);
		
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
		while (!quit)
		{
			float rec, send, temp, cpu;
			temp=pm.GetTemperature();
			cpu=pm.GetCPUUsage();
			pm.GetNetworkUsage(rec,send);
			
			PiGlowBar(0, (temp-40.0)/40.0, ledBrightness);
			PiGlowBar(1, cpu/100.0, ledBrightness);
			PiGlowBar(2, (log((rec+send)/1e2)/log(10))/6.0, ledBrightness);
			
			sleep(1);
		}
		
		// Turn off all LEDs on quit
		for (int i=0; i<6; i++) for(int l=0; l<3; l++) piGlow1(l, i, 0);
	}
	catch (runtime_error &re)
	{
		cerr << re.what()<< endl;
		return 1;
	}	
	return 0;
};
