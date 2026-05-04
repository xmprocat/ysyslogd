'use strict';
'require view';
'require form';
'require ui';

function callAction(action) {
    return L.Request.post(L.url('admin/system/diagnosis', action), {});
}

return view.extend({
    handleSaveApply: function(ev, mode) {
        return this.map.save().then(L.bind(function() {
            return callAction('apply');
        }, this)).then(L.bind(function() {
            ui.addNotification(null, _('Settings saved and applied to syslogd'), 'info');
            this.injectCustomSections();
        }, this)).catch(function(e) {
            ui.addNotification(null, _('Save failed: %s').format(e.message || ''), 'error');
        });
    },

    handleExport: function() {
        callAction('export').then(function(res) {
            var data = res.json();
            if (data.url)
                location.href = data.url;
            else
                ui.addNotification(null, _('Export failed: %s').format(data.error || ''), 'error');
        }).catch(function() {
            ui.addNotification(null, _('Export failed'), 'error');
        });
    },

    handleStatus: function() {
        callAction('status').then(function(res) {
            var data = res.json();
            var el = document.getElementById('status-text');
            if (el) el.textContent = data.status || _('Unknown');
        }).catch(function() {
            var el = document.getElementById('status-text');
            if (el) el.textContent = _('Error');
        });
    },

    injectCustomSections: function() {
        var container = document.querySelector('#maincontent .cbi-map');
        if (!container) return;

        /* Status section */
        if (!document.getElementById('status-text')) {
            var statusEl = E('div', { class: 'cbi-section' }, [
                E('h3', _('Daemon Status')),
                E('div', { class: 'cbi-value' }, [
                    E('label', { class: 'cbi-value-title' }, _('syslogd')),
                    E('div', { class: 'cbi-value-field' }, [
                        E('span', { id: 'status-text' }, _('Checking...'))
                    ])
                ])
            ]);
            container.insertBefore(statusEl, container.firstChild);
        }

        /* Export section */
        if (!document.getElementById('btn-export-logs')) {
            var exportEl = E('div', { class: 'cbi-section' }, [
                E('h3', _('Log Export')),
                E('div', { class: 'cbi-value' }, [
                    E('label', { class: 'cbi-value-title' }, _('Log Files')),
                    E('div', { class: 'cbi-value-field' }, [
                        E('button', {
                            id: 'btn-export-logs',
                            class: 'cbi-button cbi-button-download',
                            click: ui.createHandlerFn(this, 'handleExport')
                        }, _('Download Logs')),
                        ' ',
                        E('span', _('Package and download current log files.'))
                    ])
                ])
            ]);
            container.appendChild(exportEl);
        }

        this.handleStatus();
    },

    render: function() {
        var m, s, o;

        m = new form.Map('diagnosis', _('System Diagnosis'),
            _('Configure syslog daemon, adjust log levels, set up remote syslog forwarding, and export logs for troubleshooting.'));

        s = m.section(form.NamedSection, 'ysyslogd', 'diagnosis', _('Log Settings'));

        o = s.option(form.ListValue, 'log_level', _('Log Level'),
            _('Messages with priority lower than this level are filtered. Level 8 (DEBUG) shows all messages.'));
        o.value(8, '8 - DEBUG');
        o.value(7, '7 - INFO');
        o.value(6, '6 - NOTICE');
        o.value(5, '5 - WARNING');
        o.value(4, '4 - ERR');
        o.value(3, '3 - CRIT');
        o.value(2, '2 - ALERT');
        o.value(1, '1 - EMERG');
        o.default = '8';

        o = s.option(form.Value, 'log_size', _('Log Size (KB)'),
            _('Maximum log file size before rotation. Set to 0 to disable rotation.'));
        o.datatype = 'uinteger';
        o.placeholder = '5120';

        o = s.option(form.Value, 'log_rotate', _('Rotated Copies'),
            _('Number of old log files to keep, 0-99.'));
        o.datatype = 'range(0, 99)';
        o.placeholder = '4';

        o = s.option(form.Flag, 'kernel_log', _('Kernel Log'),
            _('Also read kernel messages from /dev/kmsg.'));

        s = m.section(form.NamedSection, 'ysyslogd', 'diagnosis', _('Remote Syslog'));

        o = s.option(form.Value, 'remote', _('Remote Server'),
            _('Forward logs to a remote syslog server. Format: [tcp://]host[:port].'));
        o.placeholder = 'tcp://192.168.1.1:514';

        o = s.option(form.Value, 'server_port', _('Listen Port'),
            _('TCP and UDP port to receive syslog from other devices. 0 = disabled.'));
        o.datatype = 'range(0, 65535)';
        o.placeholder = '0';

        m.handleSaveApply = L.bind(this.handleSaveApply, this);

        this.map = m;

        /* Auto-refresh every 5s */
        this._statusTimer = window.setInterval(L.bind(this.handleStatus, this), 5000);

        /* Inject custom sections after initial render */
        window.setTimeout(L.bind(this.injectCustomSections, this), 300);

        return m.render();
    },

    unload: function() {
        if (this._statusTimer) {
            window.clearInterval(this._statusTimer);
            this._statusTimer = null;
        }
    }
});
