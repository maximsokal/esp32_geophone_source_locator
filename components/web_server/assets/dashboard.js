function el(id) {
    return document.getElementById(id);
}

function text(id, value) {
    var node = el(id);

    if (node) {
        node.textContent = value;
    }
}

function fixed(value, digits, suffix, valid) {
    if (!valid) {
        return '--';
    }

    return Number(value).toFixed(digits) + suffix;
}

function formatUptime(ms) {
    var total = Math.floor(ms / 1000);
    var days = Math.floor(total / 86400);
    var hours = Math.floor((total % 86400) / 3600);
    var minutes = Math.floor((total % 3600) / 60);
    var seconds = total % 60;

    if (days > 0) {
        return days + 'd ' + hours + 'h ' + minutes + 'm';
    }

    if (hours > 0) {
        return hours + 'h ' + minutes + 'm ' + seconds + 's';
    }

    return minutes + 'm ' + seconds + 's';
}

function setClass(id, className) {
    var node = el(id);

    if (node) {
        node.className = className;
    }
}

function podStateClass(state) {
    if (state === 'READY') {
        return 'pod-state ok';
    }

    if (state === 'WARNING') {
        return 'pod-state warning';
    }

    if (state === 'ERROR') {
        return 'pod-state error';
    }

    return 'pod-state muted';
}

function updateSystem(system) {
    text(
        'system-state',
        system.state
    );

    text(
        'system-mode',
        system.mode
    );

    text(
        'system-uptime',
        formatUptime(system.uptime_ms)
    );

    setClass(
        'system-state',
        'value ok'
    );
}

function updatePower(power) {
    text(
        'power-voltage',
        fixed(
            power.voltage_v,
            2,
            ' V',
            power.valid
        )
    );

    text(
        'power-current',
        fixed(
            power.current_a,
            3,
            ' A',
            power.valid
        )
    );

    text(
        'power-watts',
        fixed(
            power.power_w,
            2,
            ' W',
            power.valid
        )
    );

    for (
        var i = 0;
        i < power.channels.length && i < 3;
        i++
    ) {
        var channel = power.channels[i];
        var n = i + 1;
        var prefix = 'power-ch' + n;

        text(
            prefix + '-status',
            channel.valid ? 'OK' : '--'
        );

        text(
            prefix + '-v',
            fixed(
                channel.voltage_v,
                2,
                ' V',
                channel.valid
            )
        );

        text(
            prefix + '-a',
            fixed(
                channel.current_a,
                3,
                ' A',
                channel.valid
            )
        );

        text(
            prefix + '-w',
            fixed(
                channel.power_w,
                2,
                ' W',
                channel.valid
            )
        );
    }
}

function updateEnvironment(environment) {
    text(
        'env-temperature',
        fixed(
            environment.temperature_c,
            1,
            ' C',
            environment.valid
        )
    );

    text(
        'env-humidity',
        fixed(
            environment.humidity_percent,
            1,
            ' %',
            environment.valid
        )
    );

    text(
        'env-pressure',
        fixed(
            environment.pressure_pa,
            1,
            ' Pa',
            environment.valid
        )
    );
}

function updateMux(mux) {
    text(
        'mux-status',
        mux.valid ? 'ACTIVE' : '--'
    );

    setClass(
        'mux-status',
        mux.valid
            ? 'value ok'
            : 'value muted'
    );

    text(
        'mux-channel',
        mux.valid && mux.active_channel >= 0
            ? String(mux.active_channel)
            : '--'
    );

    text(
        'mux-pod',
        mux.valid && mux.active_pod >= 0
            ? String(mux.active_pod + 1)
            : '--'
    );
}

function updatePod(pod) {
    var n = pod.id;
    var prefix = 'pod' + n;

    text(
        prefix + '-state',
        pod.state
    );

    setClass(
        prefix + '-state',
        podStateClass(pod.state)
    );

    var barometer = pod.barometer;

    text(
        prefix + '-pressure',
        fixed(
            barometer.pressure_pa,
            1,
            ' Pa',
            barometer.valid
        )
    );

    text(
        prefix + '-height',
        fixed(
            barometer.relative_altitude_cm,
            1,
            ' cm',
            barometer.valid
        )
    );

    var imu = pod.imu;

    text(
        prefix + '-roll',
        fixed(
            imu.roll_deg,
            1,
            ' deg',
            imu.valid
        )
    );

    text(
        prefix + '-pitch',
        fixed(
            imu.pitch_deg,
            1,
            ' deg',
            imu.valid
        )
    );

    text(
        prefix + '-yaw',
        fixed(
            imu.yaw_relative_deg,
            1,
            ' deg',
            imu.valid
        )
    );

    var geophone = pod.geophone;

    text(
        prefix + '-instant',
        fixed(
            geophone.instant_uv,
            1,
            ' uV',
            geophone.valid
        )
    );

    text(
        prefix + '-rms',
        fixed(
            geophone.rms_uv,
            1,
            ' uV',
            geophone.valid
        )
    );

    text(
        prefix + '-peak',
        fixed(
            geophone.peak_uv,
            1,
            ' uV',
            geophone.valid
        )
    );

    text(
        prefix + '-noise',
        fixed(
            geophone.noise_rms_uv,
            1,
            ' uV',
            geophone.valid
        )
    );

    text(
        prefix + '-snr',
        fixed(
            geophone.snr_db,
            1,
            ' dB',
            geophone.valid
        )
    );
}

function updatePods(pods) {
    for (
        var i = 0;
        i < pods.length;
        i++
    ) {
        updatePod(pods[i]);
    }
}

function updateAcquisition(acquisition) {
    text(
        'acq-running',
        acquisition.running
            ? 'RECORDING'
            : 'IDLE'
    );

    setClass(
        'acq-running',
        acquisition.running
            ? 'value ok'
            : 'value'
    );

    text(
        'acq-rate',
        acquisition.sample_rate_hz > 0
            ? acquisition.sample_rate_hz + ' Hz'
            : '--'
    );

    text(
        'acq-samples',
        String(acquisition.samples_recorded)
    );

    text(
        'acq-time',
        acquisition.recording_ms > 0
            ? fixed(
                acquisition.recording_ms / 1000,
                1,
                ' s',
                true
            )
            : '--'
    );
}

function updateDashboard(data) {
    updateSystem(data.system);
    updatePower(data.power);
    updateEnvironment(data.environment);
    updateMux(data.mux);
    updatePods(data.pods);
    updateAcquisition(data.acquisition);
}

function setApiOnline(online) {
    if (online) {
        text(
            'api-state',
            'LIVE'
        );

        setClass(
            'api-state',
            'api-state ok'
        );

        return;
    }

    text(
        'api-state',
        'OFFLINE'
    );

    setClass(
        'api-state',
        'api-state error'
    );
}

function refreshStatus() {
    fetch(
        '/api/status',
        {cache: 'no-store'}
    )
        .then(function(response) {
            if (!response.ok) {
                throw new Error(
                    'HTTP ' + response.status
                );
            }

            return response.json();
        })
        .then(function(data) {
            updateDashboard(data);
            setApiOnline(true);
        })
        .catch(function() {
            setApiOnline(false);
        });
}

refreshStatus();

setInterval(
    refreshStatus,
    1000
);
