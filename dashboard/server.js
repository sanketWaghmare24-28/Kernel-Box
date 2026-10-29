const express = require('express');
const { execFile } = require('child_process');
const http = require('http');
const path = require('path');
const os = require('os');
const fs = require('fs');

const PORT = 8080;

const validContainerId = (id) => /^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$/.test(id);

const readContainerConfig = (containersDir, id) => {
    const config = { ip: null, hostPort: null, containerPort: null, status: null };
    try {
        const stateFile = path.join(containersDir, id, 'state.json');
        if (fs.existsSync(stateFile)) {
            const state = JSON.parse(fs.readFileSync(stateFile, 'utf8'));
            config.status = state.status;
        }
    } catch (_) {}

    try {
        const netFile = path.join(containersDir, id, 'network.conf');
        if (fs.existsSync(netFile)) {
            const raw = fs.readFileSync(netFile, 'utf8').trim();
            if (/^10\.0\.100\.\d{1,3}$/.test(raw)) {
                config.ip = raw;
            }
        }
    } catch (_) {}

    try {
        const portFile = path.join(containersDir, id, 'ports.conf');
        if (fs.existsSync(portFile)) {
            const lines = fs.readFileSync(portFile, 'utf8').split('\n');
            for (const line of lines) {
                const trimmed = line.trim();
                const colon = trimmed.indexOf(':');
                if (colon !== -1) {
                    const hp = trimmed.substring(0, colon);
                    const cp = trimmed.substring(colon + 1);
                    if (/^\d+$/.test(hp) && /^\d+$/.test(cp)) {
                        config.hostPort = parseInt(hp, 10);
                        config.containerPort = parseInt(cp, 10);
                        break;
                    }
                }
            }
        }
    } catch (_) {}

    return config;
};

const createApp = (options = {}) => {
    const execFileImpl = typeof options === 'function' ? options : (options.execFileImpl || execFile);
    const containersDir = (typeof options === 'object' && options.containersDir) || '/var/lib/kernelbox/containers';
    const httpGetImpl = (typeof options === 'object' && options.httpGetImpl) || http.get;

    const app = express();
    app.use(express.json());
    app.use(express.static(path.join(__dirname, 'public')));

    const runCmd = (command, args = []) => {
        return new Promise((resolve) => {
            execFileImpl('sudo', ['../mini', command, ...args], { cwd: __dirname, timeout: 5000 }, (error, stdout, stderr) => {
                if (error) {
                    if (error.killed || error.signal === 'SIGTERM') {
                        console.error(`Timeout executing ${command}`);
                        return resolve({ success: false, output: 'Command timed out' });
                    }
                    console.error(`Error executing ${command}:`, stderr || error.message);
                    return resolve({ success: false, output: (stderr || error.message || '').trim() });
                }
                resolve({ success: true, output: (stdout || '').trim() });
            });
        });
    };

// 1. Get all containers
app.get('/api/containers', async (req, res) => {
    const { success, output } = await runCmd('ps');
    if (!success && !output) {
        return res.status(500).json({ error: 'Failed to list containers' });
    }

    const lines = output.split('\n');
    const containers = [];

    // Header line format: CONTAINER ID     IMAGE               STATUS              NAMES
    for (let i = 1; i < lines.length; i++) {
        const line = lines[i].trim();
        if (!line) continue;

        const parts = line.split(/\s{2,}/);
        if (parts.length >= 4) {
            containers.push({
                id: parts[0].trim(),
                image: parts[1].trim(),
                status: parts[2].trim(),
                name: parts.slice(3).join(' ').trim()
            });
        } else if (parts.length === 3) {
            containers.push({
                id: parts[0].trim(),
                image: parts[1].trim(),
                status: parts[2].trim(),
                name: ''
            });
        }
    }

    res.json(containers);
});

// 2. Get resource stats (cgroups v2)
app.get('/api/stats/:id', async (req, res) => {
    if (!validContainerId(req.params.id)) {
        return res.status(400).json({ error: 'Invalid container ID' });
    }
    const { success, output } = await runCmd('stats', [req.params.id]);
    if (!success) {
        return res.status(404).json({ error: 'Failed to fetch stats' });
    }

    const stats = {
        id: req.params.id,
        cpu_ms: 0,
        cpu_raw: '0 ms',
        memory_mb: 0,
        memory_limit_mb: 0,
        memory_raw: '0 MB',
        procs: 0
    };

    const lines = output.split('\n');
    for (const line of lines) {
        if (line.startsWith('CPU Usage:')) {
            const raw = line.replace('CPU Usage:', '').trim();
            stats.cpu_raw = raw;
            const ms = parseInt(raw, 10);
            if (!isNaN(ms)) stats.cpu_ms = ms;
        } else if (line.startsWith('Memory:')) {
            const raw = line.replace('Memory:', '').trim();
            stats.memory_raw = raw;
            const parts = raw.split('/');
            const used = parseInt(parts[0], 10);
            if (!isNaN(used)) stats.memory_mb = used;
            if (parts.length > 1) {
                const limit = parseInt(parts[1], 10);
                if (!isNaN(limit)) stats.memory_limit_mb = limit;
            }
        } else if (line.startsWith('Processes:')) {
            const p = parseInt(line.replace('Processes:', '').trim(), 10);
            if (!isNaN(p)) stats.procs = p;
        }
    }

    res.json(stats);
});

// 3. Get container logs
app.get('/api/logs/:id', async (req, res) => {
    if (!validContainerId(req.params.id)) {
        return res.status(400).json({ error: 'Invalid container ID' });
    }
    const { success, output } = await runCmd('logs', [req.params.id]);
    if (!success) {
        return res.status(404).json({ error: 'Failed to fetch logs' });
    }

    const rawLines = output.split('\n');
    const parsedLogs = rawLines.map((line, idx) => {
        // [2026-01-15T10:30:00Z] [stdout] line content OR raw text
        const match = line.match(/^\[(.*?)\]\s*\[(.*?)\]\s*(.*)$/);
        if (match) {
            return {
                id: idx,
                timestamp: match[1],
                stream: match[2].toLowerCase(),
                message: match[3]
            };
        }
        return {
            id: idx,
            timestamp: new Date().toISOString(),
            stream: 'stdout',
            message: line
        };
    });

    res.json({ logs: parsedLogs, raw: output });
});

// 4. Probe live backend health endpoint directly
app.get('/api/health/:id', async (req, res) => {
    if (!validContainerId(req.params.id)) {
        return res.status(400).json({ error: 'Invalid container ID' });
    }

    const conf = readContainerConfig(containersDir, req.params.id);
    if (conf.status !== undefined && conf.status !== null && conf.status !== 1) {
        return res.json({
            healthy: false,
            error: 'Container is not in running state',
            hint: 'Container might be stopped or exited.'
        });
    }

    if (!conf.ip && !conf.hostPort) {
        return res.json({
            healthy: false,
            error: 'No network or port configuration found for container',
            hint: 'Verify container was started with --publish or network configured.'
        });
    }

    const testEndpoint = (host, port, endpointPath) => {
        return new Promise((resolve, reject) => {
            const startTime = Date.now();
            const request = httpGetImpl({
                host,
                port,
                path: endpointPath,
                timeout: 1500
            }, (response) => {
                let data = '';
                response.on('data', chunk => data += chunk);
                response.on('end', () => {
                    const latencyMs = Date.now() - startTime;
                    try {
                        const json = JSON.parse(data);
                        resolve({ success: true, target: `${host}:${port}`, statusCode: response.statusCode, latencyMs, data: json });
                    } catch {
                        resolve({ success: true, target: `${host}:${port}`, statusCode: response.statusCode, latencyMs, data });
                    }
                });
            });

            request.on('error', (err) => reject(err));
            request.on('timeout', () => {
                request.destroy();
                reject(new Error('Request timed out'));
            });
        });
    };

    try {
        let result = null;
        if (conf.ip && conf.containerPort) {
            result = await testEndpoint(conf.ip, conf.containerPort, '/health')
                .catch(() => conf.hostPort ? testEndpoint('127.0.0.1', conf.hostPort, '/health') : Promise.reject(new Error('Direct IP probe failed')));
        } else if (conf.hostPort) {
            result = await testEndpoint('127.0.0.1', conf.hostPort, '/health');
        } else if (conf.ip) {
            result = await testEndpoint(conf.ip, 3000, '/health');
        }

        return res.json({ healthy: true, ...result });
    } catch (err) {
        return res.json({
            healthy: false,
            error: err.message || 'Connection refused',
            hint: 'Container service might be booting, crashed, or unreachable.'
        });
    }
});

// 5. Container lifecycle controls
app.post('/api/containers/:id/start', async (req, res) => {
    if (!validContainerId(req.params.id)) {
        return res.status(400).json({ error: 'Invalid container ID' });
    }
    const { success, output } = await runCmd('start', [req.params.id]);
    res.json({ success, output });
});

app.post('/api/containers/:id/stop', async (req, res) => {
    if (!validContainerId(req.params.id)) {
        return res.status(400).json({ error: 'Invalid container ID' });
    }
    const { success, output } = await runCmd('stop', [req.params.id]);
    res.json({ success, output });
});

app.post('/api/containers/:id/restart', async (req, res) => {
    if (!validContainerId(req.params.id)) {
        return res.status(400).json({ error: 'Invalid container ID' });
    }
    const { success, output } = await runCmd('restart', [req.params.id]);
    res.json({ success, output });
});

app.post('/api/containers/:id/rm', async (req, res) => {
    if (!validContainerId(req.params.id)) {
        return res.status(400).json({ error: 'Invalid container ID' });
    }
    const { success, output } = await runCmd('rm', [req.params.id]);
    res.json({ success, output });
});

app.get('/api/containers/:id/inspect', async (req, res) => {
    if (!validContainerId(req.params.id)) {
        return res.status(400).json({ error: 'Invalid container ID' });
    }
    const { success, output } = await runCmd('inspect', [req.params.id]);
    if (!success) {
        return res.status(500).json({ error: 'Failed to inspect container', output });
    }
    try {
        res.json(JSON.parse(output));
    } catch (_) {
        res.json({ raw: output });
    }
});

app.post('/api/containers/:id/pause', async (req, res) => {
    if (!validContainerId(req.params.id)) {
        return res.status(400).json({ error: 'Invalid container ID' });
    }
    const { success, output } = await runCmd('pause', [req.params.id]);
    res.json({ success, output });
});

app.post('/api/containers/:id/unpause', async (req, res) => {
    if (!validContainerId(req.params.id)) {
        return res.status(400).json({ error: 'Invalid container ID' });
    }
    const { success, output } = await runCmd('unpause', [req.params.id]);
    res.json({ success, output });
});

// 6. System & Architecture info for presentation
app.get('/api/system', (req, res) => {
    res.json({
        runtime: 'KernelBox Container Runtime v1.0',
        author: 'Student Mini Project',
        kernel: os.release(),
        arch: os.arch(),
        uptime_sec: os.uptime(),
        cgroups: 'v2 (/sys/fs/cgroup/kernelbox)',
        bridge: 'kb0 (10.0.100.1/24)',
        isolation: [
            'PID Namespace (CLONE_NEWPID)',
            'Mount Namespace (CLONE_NEWNS + pivot_root)',
            'Network Namespace (CLONE_NEWNET + veth pairs)',
            'UTS Namespace (CLONE_NEWUTS)',
            'IPC Namespace (CLONE_NEWIPC)',
            'Cgroups v2 (cpu.max + memory.max + cgroup.freeze)'
        ]
    });
});

    return app;
};

if (require.main === module) {
    createApp().listen(PORT, () => {
        console.log(`KernelBox Enhanced Dashboard running at http://localhost:${PORT}`);
    });
}

module.exports = { createApp };
