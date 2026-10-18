const express = require('express');
const { exec } = require('child_process');
const http = require('http');
const path = require('path');
const os = require('os');
const fs = require('fs');

const app = express();
const PORT = 8080;

app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// Helper to run shell commands on host
const runCmd = (cmd) => {
    return new Promise((resolve) => {
        exec(`sudo ../mini ${cmd}`, { cwd: __dirname }, (error, stdout, stderr) => {
            if (error) {
                console.error(`Error executing ${cmd}:`, stderr || error.message);
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
                id: parts[0],
                image: parts[1],
                status: parts[2],
                name: parts[3]
            });
        }
    }

    res.json(containers);
});

// 2. Get resource stats (cgroups v2)
app.get('/api/stats/:id', async (req, res) => {
    const { success, output } = await runCmd(`stats ${req.params.id}`);
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
            stats.cpu_ms = parseFloat(raw) || 0;
        } else if (line.startsWith('Memory:')) {
            const raw = line.replace('Memory:', '').trim();
            stats.memory_raw = raw;
            // Examples: "15 MB / 128 MB" or "15 MB"
            const match = raw.match(/(\d+(?:\.\d+)?)\s*MB(?:\s*\/\s*(\d+(?:\.\d+)?)\s*MB)?/i);
            if (match) {
                stats.memory_mb = parseFloat(match[1]) || 0;
                stats.memory_limit_mb = match[2] ? parseFloat(match[2]) : 128;
            }
        } else if (line.startsWith('Processes:')) {
            stats.procs = parseInt(line.replace('Processes:', '').trim(), 10) || 0;
        }
    }

    res.json(stats);
});

// 3. Get container logs
app.get('/api/logs/:id', async (req, res) => {
    const { success, output } = await runCmd(`logs ${req.params.id}`);
    if (!success) {
        return res.status(404).json({ error: 'Could not fetch logs', logs: [] });
    }

    const rawLines = output.split('\n').filter(Boolean);
    const parsedLogs = rawLines.map((line, idx) => {
        // Format: [2024-01-15T10:30:00Z] [stdout] line content OR raw text
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
    // Try pinging 10.0.100.2:3000/health then 127.0.0.1:3000/health
    const testEndpoint = (host, port, endpointPath) => {
        return new Promise((resolve, reject) => {
            const startTime = Date.now();
            const request = http.get({
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
                        resolve({ success: true, statusCode: response.statusCode, latencyMs, data: json });
                    } catch {
                        resolve({ success: true, statusCode: response.statusCode, latencyMs, data });
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
        const result = await testEndpoint('10.0.100.2', 3000, '/health')
            .catch(() => testEndpoint('127.0.0.1', 3000, '/health'));
        return res.json({ healthy: true, ...result });
    } catch (err) {
        return res.json({
            healthy: false,
            error: err.message || 'Connection refused',
            hint: 'Container might be booting or stopped.'
        });
    }
});

// 5. Container lifecycle controls
app.post('/api/containers/:id/stop', async (req, res) => {
    const { success, output } = await runCmd(`stop ${req.params.id}`);
    res.json({ success, output });
});

app.post('/api/containers/:id/rm', async (req, res) => {
    const { success, output } = await runCmd(`rm ${req.params.id}`);
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
            'Cgroups v2 (cpu.max + memory.max)'
        ]
    });
});

app.listen(PORT, () => {
    console.log(`KernelBox Enhanced Dashboard running at http://localhost:${PORT}`);
});
