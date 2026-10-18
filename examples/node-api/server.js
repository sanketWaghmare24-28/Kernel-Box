// KernelBox Demo Node.js API
// A minimal HTTP server to demonstrate containerized application deployment

const http = require('http');
const os = require('os');

const PORT = process.env.PORT || 3000;
const NODE_ENV = process.env.NODE_ENV || 'development';

const server = http.createServer((req, res) => {
    const timestamp = new Date().toISOString();
    
    if (req.url === '/health' && req.method === 'GET') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
            status: 'healthy',
            runtime: 'KernelBox',
            timestamp: timestamp,
            uptime: process.uptime(),
            environment: NODE_ENV
        }));
    } else if (req.url === '/info' && req.method === 'GET') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
            runtime: 'KernelBox Container Runtime',
            hostname: os.hostname(),
            platform: os.platform(),
            arch: os.arch(),
            nodeVersion: process.version,
            pid: process.pid,
            memoryUsage: process.memoryUsage(),
            cpuUsage: process.cpuUsage(),
            environment: NODE_ENV,
            timestamp: timestamp
        }));
    } else if (req.url === '/' && req.method === 'GET') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
            message: 'Welcome to KernelBox Demo API!',
            version: '1.0.0',
            endpoints: [
                { method: 'GET', path: '/', description: 'This help message' },
                { method: 'GET', path: '/health', description: 'Health check' },
                { method: 'GET', path: '/info', description: 'System information' }
            ]
        }));
    } else {
        res.writeHead(404, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'Not found' }));
    }
    
    console.log(`${timestamp} ${req.method} ${req.url} ${res.statusCode}`);
});

server.listen(PORT, '0.0.0.0', () => {
    console.log(`KernelBox Demo API running on port ${PORT}`);
    console.log(`Environment: ${NODE_ENV}`);
    console.log(`PID: ${process.pid}`);
    console.log(`Hostname: ${os.hostname()}`);
});

// Graceful shutdown
process.on('SIGTERM', () => {
    console.log('Received SIGTERM, shutting down gracefully...');
    server.close(() => {
        console.log('Server closed');
        process.exit(0);
    });
});

process.on('SIGINT', () => {
    console.log('Received SIGINT, shutting down...');
    server.close(() => {
        process.exit(0);
    });
});
