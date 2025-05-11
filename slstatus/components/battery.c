/* See LICENSE file for copyright and license details. */
#include <stdio.h>
#include <string.h>

#include "../slstatus.h"
#include "../util.h"

#if defined(__linux__)
/*
 * https://www.kernel.org/doc/html/latest/power/power_supply_class.html
 */
	#include <limits.h>
	#include <stdint.h>
	#include <unistd.h>

	#define POWER_SUPPLY_CAPACITY		"/sys/class/power_supply/%s/capacity"
	#define POWER_SUPPLY_STATUS			"/sys/class/power_supply/%s/status"
	#define POWER_SUPPLY_CHARGE			"/sys/class/power_supply/%s/charge_now"
	#define POWER_SUPPLY_CHARGE_FULL	"/sys/class/power_supply/%s/charge_full"
	#define POWER_SUPPLY_ENERGY			"/sys/class/power_supply/%s/energy_now"
	#define POWER_SUPPLY_ENERGY_FULL	"/sys/class/power_supply/%s/energy_full"
	#define POWER_SUPPLY_CURRENT		"/sys/class/power_supply/%s/current_now"
	#define POWER_SUPPLY_POWER			"/sys/class/power_supply/%s/power_now"

	static const char *
	pick(const char *bat, const char *f1, const char *f2, char *path,
	     size_t length)
	{
		if (esnprintf(path, length, f1, bat) > 0 &&
		    access(path, R_OK) == 0)
			return f1;

		if (esnprintf(path, length, f2, bat) > 0 &&
		    access(path, R_OK) == 0)
			return f2;

		return NULL;
	}

	const char *
	battery_perc(const char *bat)
	{
		int cap_perc;
		char path[PATH_MAX];

		if (esnprintf(path, sizeof(path), POWER_SUPPLY_CAPACITY, bat) < 0)
			return NULL;
		if (pscanf(path, "%d", &cap_perc) != 1)
			return NULL;

		return bprintf("%d", cap_perc);
	}

	const char *
	battery_state(const char *bat)
	{
		static struct {
			char *state;
			char *symbol;
		} map[] = {
			{ "Charging",    "+" },
			{ "Discharging", "-" },
			{ "Full",        "o" },
			{ "Not charging", "o" },
		};
		size_t i;
		char path[PATH_MAX], state[12];

		if (esnprintf(path, sizeof(path), POWER_SUPPLY_STATUS, bat) < 0)
			return NULL;
		if (pscanf(path, "%12[a-zA-Z ]", state) != 1)
			return NULL;

		for (i = 0; i < LEN(map); i++)
			if (!strcmp(map[i].state, state))
				break;

		return (i == LEN(map)) ? "?" : map[i].symbol;
	}

	/* Modified to show time left until battery is full */
	const char *
	battery_remaining(const char *bat)
	{
		/* Replaced
		 * charge_now -> energy_now
		 * current_now -> power_now
		 * and added energy_full for charging and discharging time */
		uintmax_t energy_now, power_now, energy_full, m, h;
		double timeleft;
		char path[PATH_MAX], state[12];
		
		/* Get power supply status */
		if (esnprintf(path, sizeof(path), POWER_SUPPLY_STATUS, bat) < 0)
			return NULL;
		if (pscanf(path, "%12[a-zA-Z ]", state) != 1)
			return NULL;

		/* Get current energy */
		if (!pick(bat, POWER_SUPPLY_ENERGY, POWER_SUPPLY_CHARGE, path,
		          sizeof(path)) ||
		    pscanf(path, "%ju", &energy_now) < 0)
			return NULL;

		/* Get energy full */
		if (!pick(bat, POWER_SUPPLY_ENERGY_FULL, POWER_SUPPLY_CHARGE_FULL, path,
		          sizeof(path)) ||
		    pscanf(path, "%ju", &energy_full) < 0)
			return NULL;
		
		/* Get power_now (charging or discharging rate) */
		if (!pick(bat, POWER_SUPPLY_POWER, POWER_SUPPLY_CURRENT, path,
			          sizeof(path)) ||
			    pscanf(path, "%ju", &power_now) < 0)
				return NULL;

		if (power_now == 0)
			return NULL;
		
		/* See if battery is discharging or charging */
		if (!strcmp(state, "Discharging")) {
			timeleft = (double)energy_now / (double)power_now;
			h = timeleft;
			m = (timeleft - (double)h) * 60;
			return bprintf("%juh %jum", h, m);
		} else if (!strcmp(state, "Charging")) {
			timeleft = (double)(energy_full - energy_now) / (double)power_now;
			h = timeleft;
			m = (timeleft - (double)h) * 60;
			return bprintf("%juh %jum", h, m);
		}

		return NULL;
}

/* OpenBSD config */
#elif defined(__OpenBSD__)
	#include <fcntl.h>
	#include <machine/apmvar.h>
	#include <sys/ioctl.h>
	#include <unistd.h>

	static int
	load_apm_power_info(struct apm_power_info *apm_info)
	{
		int fd;

		fd = open("/dev/apm", O_RDONLY);
		if (fd < 0) {
			warn("open '/dev/apm':");
			return 0;
		}

		memset(apm_info, 0, sizeof(struct apm_power_info));
		if (ioctl(fd, APM_IOC_GETPOWER, apm_info) < 0) {
			warn("ioctl 'APM_IOC_GETPOWER':");
			close(fd);
			return 0;
		}
		return close(fd), 1;
	}

	const char *
	battery_perc(const char *unused)
	{
		struct apm_power_info apm_info;

		if (load_apm_power_info(&apm_info))
			return bprintf("%d", apm_info.battery_life);

		return NULL;
	}

	const char *
	battery_state(const char *unused)
	{
		struct {
			unsigned int state;
			char *symbol;
		} map[] = {
			{ APM_AC_ON,      "+" },
			{ APM_AC_OFF,     "-" },
		};
		struct apm_power_info apm_info;
		size_t i;

		if (load_apm_power_info(&apm_info)) {
			for (i = 0; i < LEN(map); i++)
				if (map[i].state == apm_info.ac_state)
					break;

			return (i == LEN(map)) ? "?" : map[i].symbol;
		}

		return NULL;
	}

	const char *
	battery_remaining(const char *unused)
	{
		struct apm_power_info apm_info;
		unsigned int h, m;

		if (load_apm_power_info(&apm_info)) {
			if (apm_info.ac_state != APM_AC_ON) {
				h = apm_info.minutes_left / 60;
				m = apm_info.minutes_left % 60;
				return bprintf("%uh %02um", h, m);
			} else {
				return "";
			}
		}

		return NULL;
	}

/* FreeBSD config */
#elif defined(__FreeBSD__)
	#include <sys/sysctl.h>

	#define BATTERY_LIFE  "hw.acpi.battery.life"
	#define BATTERY_STATE "hw.acpi.battery.state"
	#define BATTERY_TIME  "hw.acpi.battery.time"

	const char *
	battery_perc(const char *unused)
	{
		int cap_perc;
		size_t len;

		len = sizeof(cap_perc);
		if (sysctlbyname(BATTERY_LIFE, &cap_perc, &len, NULL, 0) < 0 || !len)
			return NULL;

		return bprintf("%d", cap_perc);
	}

	const char *
	battery_state(const char *unused)
	{
		int state;
		size_t len;

		len = sizeof(state);
		if (sysctlbyname(BATTERY_STATE, &state, &len, NULL, 0) < 0 || !len)
			return NULL;

		switch (state) {
		case 0: /* FALLTHROUGH */
		case 2:
			return "+";
		case 1:
			return "-";
		default:
			return "?";
		}
	}

	const char *
	battery_remaining(const char *unused)
	{
		int rem;
		size_t len;

		len = sizeof(rem);
		if (sysctlbyname(BATTERY_TIME, &rem, &len, NULL, 0) < 0 || !len
		    || rem < 0)
			return NULL;

		return bprintf("%uh %02um", rem / 60, rem % 60);
	}
#endif
