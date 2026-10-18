const assert = require('node:assert/strict');
const http = require('node:http');
const test = require('node:test');
const { createApp } = require('./server');

const request = (server, method, requestPath) => new Promise((resolve, reject) => {
    const address = server.address();
    const req = http.request({
        host: address.address,
        port: address.port,
        method,
        path: requestPath
    }, (res) => {
        let body = '';
        res.setEncoding('utf8');
        res.on('data', chunk => body += chunk);
        res.on('end', () => resolve({ status: res.statusCode, body: JSON.parse(body) }));
    });
    req.on('error', reject);
    req.end();
});

test('dashboard passes the container ID as a separate, validated argument', async (t) => {
    const calls = [];
    const server = createApp((file, args, options, callback) => {
        calls.push({ file, args, options });
        callback(null, 'CPU Usage: 1 ms\nMemory: 2 MB / 128 MB\nProcesses: 1', '');
    }).listen(0, '127.0.0.1');
    await new Promise(resolve => server.once('listening', resolve));
    t.after(() => new Promise(resolve => server.close(resolve)));

    const response = await request(server, 'GET', '/api/stats/abc123');
    assert.equal(response.status, 200);
    assert.equal(response.body.id, 'abc123');
    assert.equal(calls.length, 1);
    assert.equal(calls[0].file, 'sudo');
    assert.deepEqual(calls[0].args, ['../mini', 'stats', 'abc123']);
    assert.equal(calls[0].options.cwd, require('node:path').join(__dirname));
});

test('dashboard rejects shell metacharacters before invoking the runtime', async (t) => {
    const calls = [];
    const server = createApp((file, args, options, callback) => {
        calls.push({ file, args, options });
        callback(null, '', '');
    }).listen(0, '127.0.0.1');
    await new Promise(resolve => server.once('listening', resolve));
    t.after(() => new Promise(resolve => server.close(resolve)));

    const response = await request(server, 'POST', '/api/containers/abc%3Btouch%20pwned/stop');
    assert.equal(response.status, 400);
    assert.deepEqual(calls, []);
});
