#!/system/bin/sh
#
#	This file is part of the OrangeFox Recovery Project
# 	Copyright (C) 2026 The OrangeFox Recovery Project
#
#	OrangeFox is free software: you can redistribute it and/or modify
#	it under the terms of the GNU General Public License as published by
#	the Free Software Foundation, either version 3 of the License, or
#	any later version.
#
#	OrangeFox is distributed in the hope that it will be useful,
#	but WITHOUT ANY WARRANTY; without even the implied warranty of
#	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#	GNU General Public License for more details.
#
# 	This software is released under GPL version 3 or any later version.
#	See <http://www.gnu.org/licenses/>.
#
# 	Please maintain this if you use this script or any part of it
#

DEBUG=0;
[ "$DEBUG" = "1" ] && set -o xtrace;

LOGMSG() {
	echo "I:$@" >> /tmp/recovery.log;
}

# prune historic logs
prune_historic_logs() {
local FOX_HOME=$(getprop "ro.orangefox.home");
local FOX_SETTINGS=$(getprop "ro.orangefox.settings");
local days="$1"; # number of days before we start pruning historic logs

	[ -z "$FOX_HOME" ] && FOX_HOME=/sdcard/Fox; # default
	[ -z "$FOX_SETTINGS" ] && FOX_SETTINGS=/sdcard/Fox; # default
	[ -z "$days" ] && days=14; # default
	local D="/sdcard/Fox/logs"; # default

	local D1=$FOX_HOME/logs;
	local D2=$FOX_SETTINGS/logs;
	if [ -d $D1 ]; then # home dir
		D=$D1;
	elif [ -d $D2 ]; then # settings dir
		D=$D2;
	fi

	# look only for the historic log zip files
	[ "$DEBUG" = "1" ] && LOGMSG "Pruning historic logs ...";
	find "$D" -name "recovery*.zip" -maxdepth 1 -type f -mtime +$days -print -delete;
}

# ---
prune_historic_logs $1;
exit 0;
#
