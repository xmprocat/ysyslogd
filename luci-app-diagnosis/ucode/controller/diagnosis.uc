'use strict';

import { open, popen, mkstemp, stat, unlink, basename } from 'fs';

const SYLOGD   = '/sbin/syslogd';
const CONFFILE = '/etc/syslog.conf';

function syslogd_get() {
	let fd = popen(`${SYLOGD} -c ${CONFFILE} --get 2>/dev/null`);
	if (!fd)
		return null;
	let out = fd.read(4096) ?? '';
	fd.close();
	return out;
}

function syslogd_set(key, value) {
	let fd = popen(`${SYLOGD} -c ${CONFFILE} --set ${key}=${value} 2>&1`);
	if (!fd)
		return null;
	let out = fd.read(1024) ?? '';
	fd.close();
	return (index(out, 'ERROR') < 0);
}

function syslogd_reload() {
	let fd = popen(`${SYLOGD} -c ${CONFFILE} --reload 2>&1`);
	if (!fd)
		return false;
	let out = fd.read(1024) ?? '';
	fd.close();
	return (index(out, 'ERROR') < 0);
}

function syncconf() {
	let keys = ['log_level', 'log_size', 'log_rotate', 'remote', 'server_port', 'kernel_log', 'kmsg'];
	let lines = ['# syslogd runtime configuration'];

	for (let k in keys) {
		let val = uci.get('diagnosis', 'ysyslogd', k) ?? '';
		push(lines, `${k}=${val}`);
	}

	let content = join('\n', lines) + '\n';

	/* Write to /etc/syslog.conf */
	let fd = open(CONFFILE, 'w');
	if (fd) {
		fd.write(content);
		fd.close();
		return true;
	}
	return false;
}

return {
	action_status: function(env) {
		let result = {};

		/* Check if syslogd is running */
		let fd = popen('pidof syslogd 2>/dev/null');
		if (fd) {
			let pid = trim(fd.read(64) ?? '');
			fd.close();
			if (pid)
				result.status = sprintf('Running (PID: %s)', pid);
			else
				result.status = 'Not running';
		} else {
			result.status = 'Not running';
		}

		/* Try to get config from syslogd */
		let cfg = syslogd_get();
		if (cfg)
			result.config = cfg;

		http.prepare_content('application/json');
		http.write_json(result);
	},

	action_apply: function(env) {
		let result = { ok: false };

		/* Sync UCI to config file */
		syncconf();

		/* Apply each setting via control socket */
		let keys = ['log_level', 'log_size', 'log_rotate', 'remote', 'server_port', 'kernel_log', 'kmsg'];
		let errors = [];

		for (let k in keys) {
			let val = uci.get('diagnosis', 'ysyslogd', k) ?? '';
			if (!syslogd_set(k, val))
				push(errors, sprintf('Failed to set %s', k));
		}

		syslogd_reload();

		result.ok = (length(errors) == 0);
		if (!result.ok)
			result.error = join('; ', errors);

		http.prepare_content('application/json');
		http.write_json(result);
	},

	action_export: function(env) {
		let result = { ok: false };
		let tmppath = sprintf('/tmp/diag_export_%d.tar.gz', time());

		/* Package log files */
		let fd = popen(sprintf(
			'tar czf %s /tmp/log/*.log 2>/dev/null; echo $?',
			tmppath
		));
		if (fd) {
			let rc = trim(fd.read(64) ?? '1');
			fd.close();

			if (rc == '0') {
				result.ok = true;
				result.url = sprintf('/cgi-bin/luci/admin/system/diagnosis/download?file=%s', tmppath);
			}
		}

		/* Fallback: gzip the log */
		if (!result.ok) {
			let fd2 = popen(sprintf(
				'cat /tmp/log/*.log 2>/dev/null | gzip > %s; echo $?',
				tmppath
			));
			if (fd2) {
				let rc = trim(fd2.read(64) ?? '1');
				fd2.close();
				if (rc == '0') {
					result.ok = true;
					result.url = sprintf('/cgi-bin/luci/admin/system/diagnosis/download?file=%s', tmppath);
				}
			}
		}

		if (!result.ok)
			result.error = 'No log files found';

		http.prepare_content('application/json');
		http.write_json(result);
	},

	action_dump: function(env) {
		let result = { ok: false };
		let tmppath = sprintf('/tmp/diag_ringbuf_%d.txt', time());

		let fd = popen(sprintf('%s -v > %s 2>/dev/null; echo $?', SYLOGD, tmppath));
		if (fd) {
			/* Wait for completion */
			fd.read(1024 * 1024);
			fd.close();
		}

		let st = stat(tmppath);
		if (st && st.size > 0) {
			result.ok = true;
			result.url = sprintf('/cgi-bin/luci/admin/system/diagnosis/download?file=%s', tmppath);
		} else {
			result.error = 'Ring buffer is empty or syslogd is not running';
		}

		http.prepare_content('application/json');
		http.write_json(result);
	},

	action_download: function(env) {
		let filepath = http.formvalue('file', true) ?? '';

		/* Only allow files in /tmp/ with diag_ prefix */
		if (index(filepath, '/tmp/diag_') != 0 || index(filepath, '..') >= 0) {
			http.status(403, 'Forbidden');
			return;
		}

		let st = stat(filepath);
		if (!st || st.size == 0) {
			http.status(404, 'File not found');
			return;
		}

		let fd = popen(sprintf('cat %s 2>/dev/null', filepath));
		if (!fd) {
			http.status(500, 'Cannot read file');
			return;
		}

		http.header('Content-Disposition', sprintf('attachment; filename="%s"', basename(filepath)));
		if (index(filepath, '.gz') >= 0 || index(filepath, '.tar') >= 0)
			http.prepare_content('application/octet-stream');
		else
			http.prepare_content('text/plain; charset=UTF-8');

		let chunk = fd.read(65536);
		while (length(chunk)) {
			http.write(chunk);
			chunk = fd.read(65536);
		}
		fd.close();

		/* Clean up temp file */
		unlink(filepath);
	}
};
