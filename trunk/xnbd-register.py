#! /usr/bin/python
# -*- coding: utf-8 -*-

# xnbd-register - a configuration interface to xnbd devices
#
# Copyright (C) 2012 Arno Toell <arno@debian.org>
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.


from __future__ import print_function

import argparse
import sys
import subprocess
import json
import re
import os.path
import types

XNBD_CLIENT = "xnbd-client"
XNBD_WRAPPER = "xnbd-wrapper"
XNBD_WRAPPER_CTL = "xnbd-wrapper-ctl"
VERBOSE = True

SERVER_KEY = 'server'
WRAPPER_KEY = 'wrapper'

WRAPPER_ADDRESS_KEY = 'address'
WRAPPER_LOGPATH_KEY = 'logpath'
WRAPPER_PORT_KEY = 'port'
WRAPPER_SOCKET_KEY = 'socket'
WRAPPER_MAX_BUF_SIZE_KEY = 'max_buf_size'
WRAPPER_MAX_QUEUE_SIZE_KEY = 'max_queue_size'
WRAPPER_VOLUMES_KEY = 'volumes'

DEFAULT_ADDRESS = '127.0.0.1'
DEFAULT_PORT = 8520
DEFAULT_LOGPATH = '/var/log/xnbd.log'
DEFAULT_SOCKET = '/var/run/xnbd-wrapper.ctl'


def vprint(msg, **kwargs):
	if (VERBOSE):
		print(msg, **kwargs)

def handle_server_wrapper_migration(conf, config_file):
	server_present = SERVER_KEY in conf
	wrapper_present = WRAPPER_KEY in conf

	if server_present and wrapper_present:
		vprint('Conflict in config file "%s": Use of both key "%s" (old, deprecated) and key "%s" (new) not supported. Please merge into "%s".' \
				% (config_file, SERVER_KEY, WRAPPER_KEY, WRAPPER_KEY))
		sys.exit(1)
	elif server_present:
		vprint('Warning for config file "%s": Use of key "%s" is deprecated, please rename to "%s". Thanks.' \
				% (config_file, SERVER_KEY, WRAPPER_KEY))
		assert not wrapper_present

		# Rename server to wrapper
		conf[WRAPPER_KEY] = conf[SERVER_KEY]
		del conf[SERVER_KEY]

	assert SERVER_KEY not in conf

def check_syntax(data, config_file):
	if (not isinstance(data, dict)):
		vprint("Invalid syntax in configuration file `%s': Expected a sequence of nbdX and/or wrapper objects")
		sys.exit(1)
	for key in data:
		if (key != WRAPPER_KEY and not key.startswith("nbd")):
			vprint("Invalid key: `%s' in configuration file `%s'" % (key, config_file))
			sys.exit(1)

		if (key == WRAPPER_KEY):
			wrapper_keys = set([
					WRAPPER_ADDRESS_KEY,
					WRAPPER_LOGPATH_KEY,
					WRAPPER_MAX_BUF_SIZE_KEY,
					WRAPPER_MAX_QUEUE_SIZE_KEY,
					WRAPPER_PORT_KEY,
					WRAPPER_SOCKET_KEY,
					WRAPPER_VOLUMES_KEY,
					])
			config_keys = set(data[key].keys())
			ukeys = config_keys - wrapper_keys
			if (ukeys):
				vprint("WARNING: Unknown wrapper option(s): %s\n" % ", ".join(ukeys))
				sys.exit(1)

		elif (key.startswith("nbd")):
			if (not re.match("nbd\d+", key)):
				vprint("Invalid NBD device: %s" % key)
				sys.exit(1)
			if (not "host" in data[key] or not "port" in data[key]):
				vprint("Incomplete NBD configuration. Was expecting `host' and `port' for NBD device `%s'" % (key))
				sys.exit(1)
			continue

		else:
			vprint("Unknown configuration object: `%s'" % key)
			sys.exit(1)


def call(command, description):
	vprint(description, end='')
	try:
		p = subprocess.Popen(command, stderr = subprocess.STDOUT)
	except OSError:
		vprint("failed")
		return 127

	p.wait()
	if (p.returncode != 0):
		vprint("failed")
		sys.exit(p.returncode)
	else:
		vprint("ok")
	return p.returncode

def start_client(device, data):
	start_cmd = [XNBD_CLIENT, '/dev/%s' % (device), '--connect']
	if ('name' in data):
		start_cmd.append("--exportname")
		start_cmd.append(data['name'])

	start_cmd.append(data['host'])
	start_cmd.append(str(data['port']))
	call(start_cmd, "Starting /dev/%s ... " % (device))

def stop_client(device, data):
	stop_cmd = [XNBD_CLIENT,  '--disconnect', '/dev/%s' % (device)]
	call(stop_cmd, "Stopping /dev/%s ... " % (device))

def start_wrapper(data):
	start_cmd = [XNBD_WRAPPER, "--daemonize",
			"--logpath", data.get(WRAPPER_LOGPATH_KEY, DEFAULT_LOGPATH),
			"--laddr", data.get(WRAPPER_ADDRESS_KEY, DEFAULT_ADDRESS),
			"--port", str(data.get(WRAPPER_PORT_KEY, DEFAULT_PORT)),
			"--socket", data.get(WRAPPER_SOCKET_KEY, DEFAULT_SOCKET),
			]

	for parameter, config_key in (
			('--max-queue-size', WRAPPER_MAX_QUEUE_SIZE_KEY),
			('--max-buf-size',   WRAPPER_MAX_BUF_SIZE_KEY),
			):
		number = int(data.get(config_key, 0))
		if number > 0:
			start_cmd.append(parameter)
			start_cmd.append(str(number))

	if call(start_cmd, "Starting `%s' ..." % (XNBD_WRAPPER)):
		sys.exit(1)

	if isinstance(data[WRAPPER_VOLUMES_KEY], types.ListType):
		# List data, format of 0.1.0-pre*
		exportname_volume_tuple_list = [(path, path) for path in data[WRAPPER_VOLUMES_KEY]]
	else:
		# Dict data, format of >=0.2.0
		exportname_volume_tuple_list = list(data[WRAPPER_VOLUMES_KEY].items())

	for exportname, volume in exportname_volume_tuple_list:
		add_volume = [XNBD_WRAPPER_CTL, "--socket", data['socket'], "--add-target", exportname, volume]
		if (os.path.exists(volume)):
			call(add_volume, "Adding `%s' ..." % volume)
		else:
			vprint("%s: Can't access volume" % (volume))

def stop_wrapper(data):
	stop = [XNBD_WRAPPER_CTL, "--socket", data['socket'], "--shutdown"]
	call(stop, "Shutting down all xnbd shares ...")

def print_status(data):
	status = [XNBD_WRAPPER_CTL, "--socket", data['socket'], "-l"]
	call(status, "")


parser = argparse.ArgumentParser(description='xNBD helper to (un)register xNBD devices')
parser.add_argument('-s', '--start', action='store_true', help='mount configured xNBD client connections and start configured xNBD wrapper')
parser.add_argument('-r', '--restart', action='store_true', help='(re-)mount configured xNBD client connections and (re-)start configured xNBD wrapper')
parser.add_argument('-t', '--stop', action='store_true', help='unmount configured xNBD client connections and stop configured xNBD wrapper')
parser.add_argument('-a', '--status', action='store_true', help='show xNBD wrapper status')
parser.add_argument('--config', dest='config_file', default='/etc/xnbd.conf', help='config file to use (default: /etc/xnbd.conf)')
parser.add_argument('--quiet', action='store_true', help='do not give verbose output')

args = parser.parse_args()

if(args.quiet):
	VERBOSE = False

try:
	conf_parser = open(args.config_file, "r")
	read_configuration = ""
	for line in conf_parser.readlines():
		line = re.sub("\#.*$", "", line) # ignore comments
		read_configuration += line

	configuration = json.loads(read_configuration)
except (IOError, OSError) as e:
	vprint("Configuration file not accessible `%s\': %s" % ( args.config_file, e ) )
	sys.exit(1)
except ValueError as e:
	vprint("Syntax error in configuration `%s': %s" % (args.config_file, e))
	sys.exit(1)

conf_parser.close()
handle_server_wrapper_migration(configuration, args.config_file)
check_syntax(configuration, args.config_file)

if ( not len(configuration) ):
	vprint("WARNING: Not starting anything")
	sys.exit(2)

wrapper_configured = WRAPPER_KEY in configuration

if (args.status):
	if (not wrapper_configured):
		vprint("WARNING: Wrapper socket unknown (since no wrapper is configured)")
		sys.exit(2)
	print_status(configuration[WRAPPER_KEY])
	sys.exit(0)

if (not args.stop and not args.restart and not args.start):
	vprint("%s: One action is required" % sys.argv[0])
	sys.exit(1)

client_device_names = [k for k in configuration.keys() if k != WRAPPER_KEY]

if (args.stop or args.restart):
	# Stop clients first, they may be using our own wrapper
	for instance in client_device_names:
		stop_client(instance, configuration[instance])

	if wrapper_configured:
		stop_wrapper(configuration[WRAPPER_KEY])

if (args.start or args.restart):
	# Start wrapper frist, it may be used our own clients
	if wrapper_configured:
		start_wrapper(configuration[WRAPPER_KEY])

	for instance in client_device_names:
		start_client(instance, configuration[instance])

sys.exit(0)
