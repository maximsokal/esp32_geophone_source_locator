const POINTS = 1000;
const REFRESH_MS = 200;
const SCALE_RELEASE = 0.92;

let lastData = null;
let displayScale = 10;

function el(id) {
    return document.getElementById(id);
}

function setStatus(text, cls) {
    el('status').textContent = text;
    el('status').className = 'status ' + cls;
}

function targetScaleFor(data) {
    let max = 0;

    data.channels.forEach(function(channel) {
        channel.samples_uv.forEach(function(value) {
            max = Math.max(max, Math.abs(value));
        });
    });

    return Math.max(10, max * 1.10);
}

function smoothScale(target) {
    if (target >= displayScale) {
        displayScale = target;
    }
    else {
        displayScale =
            Math.max(
                target,
                displayScale * SCALE_RELEASE
            );
    }

    return displayScale;
}

function drawGraph(id, samples, scale) {
    const canvas = el(id);
    const ratio = window.devicePixelRatio || 1;
    const width = Math.max(280, canvas.clientWidth);
    const height = 120;

    canvas.width = Math.floor(width * ratio);
    canvas.height = Math.floor(height * ratio);

    const ctx = canvas.getContext('2d');
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);

    ctx.clearRect(0, 0, width, height);

    ctx.strokeStyle = '#21262d';
    ctx.lineWidth = 1;

    for (let i = 1; i < 4; i++) {
        const y = height * i / 4;

        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(width, y);
        ctx.stroke();
    }

    ctx.strokeStyle = '#30363d';
    ctx.beginPath();
    ctx.moveTo(0, height / 2);
    ctx.lineTo(width, height / 2);
    ctx.stroke();

    if (samples.length < 2) {
        return;
    }

    ctx.strokeStyle = '#58a6ff';
    ctx.lineWidth = 1.5;
    ctx.beginPath();

    samples.forEach(function(value, index) {
        const x =
            index /
            (samples.length - 1) *
            width;

        const y =
            height / 2 -
            (value / scale) *
            (height * 0.42);

        if (index === 0) {
            ctx.moveTo(x, y);
        }
        else {
            ctx.lineTo(x, y);
        }
    });

    ctx.stroke();
}

function render(data) {
    lastData = data;

    el('source').textContent = data.source;

    el('rate').textContent =
        data.sample_rate_hz + ' Hz';

    const seconds =
        data.sample_rate_hz > 0
            ? data.count /
                data.sample_rate_hz
            : 0;

    el('window').textContent =
        seconds.toFixed(2) + ' s';

    el('frames').textContent =
        data.total_frames;

    if (data.source === 'TEST') {
        setStatus(
            'TEST SIGNAL',
            'warning'
        );
    }
    else if (
        data.source ===
        'ESP_ADC_BENCH'
    ) {
        setStatus(
            'BENCH / ESP ADC',
            'warning'
        );
    }
    else if (
        data.source ===
        'AD7606'
    ) {
        setStatus(
            'LIVE',
            'ok'
        );
    }
    else {
        setStatus(
            'WAITING',
            'muted'
        );
    }

    const scale =
        smoothScale(
            targetScaleFor(data)
        );

    data.channels.forEach(function(channel) {
        drawGraph(
            'graph' + channel.id,
            channel.samples_uv,
            scale
        );

        el(
            'range' + channel.id
        ).textContent =
            '+/- ' +
            scale.toFixed(1) +
            ' uV';
    });
}

let refreshInFlight = false;

function refresh() {
    if (refreshInFlight) {
        return;
    }

    refreshInFlight = true;

    fetch(
        '/api/seismic?points=' + POINTS,
        {cache: 'no-store'}
    )
        .then(function(response) {
            if (!response.ok) {
                throw new Error();
            }

            return response.json();
        })
        .then(function(data) {
            render(data);
        })
        .catch(function() {
            setStatus(
                'OFFLINE',
                'error'
            );
        })
        .finally(function() {
            refreshInFlight = false;
        });
}

window.addEventListener(
    'resize',
    function() {
        if (lastData) {
            render(lastData);
        }
    }
);

refresh();
setInterval(
    refresh,
    REFRESH_MS
);
