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

test('health endpoint rejects invalid container ID', async (t) => {
    const server = createApp().listen(0, '127.0.0.1');
    await new Promise(resolve => server.once('listening', resolve));
    t.after(() => new Promise(resolve => server.close(resolve)));

    const response = await request(server, 'GET', '/api/health/bad%20id%24');
    assert.equal(response.status, 400);
    assert.equal(response.body.error, 'Invalid container ID');
});

test('health endpoint reports not running when state indicates container stopped', async (t) => {
    const fs = require('node:fs');
    const os = require('node:os');
    const path = require('node:path');
    const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'kb-health-test-'));
    t.after(() => fs.rmSync(tempDir, { recursive: true, force: true }));

    const containerId = 'test01020304';
    const cdir = path.join(tempDir, containerId);
    fs.mkdirSync(cdir, { recursive: true });
    fs.writeFileSync(path.join(cdir, 'state.json'), JSON.stringify({ id: containerId, status: 2 }));

    const server = createApp({ containersDir: tempDir }).listen(0, '127.0.0.1');
    await new Promise(resolve => server.once('listening', resolve));
    t.after(() => new Promise(resolve => server.close(resolve)));

    const response = await request(server, 'GET', `/api/health/${containerId}`);
    assert.equal(response.status, 200);
    assert.equal(response.body.healthy, false);
    assert.match(response.body.error, /not in running state/);
});

test('health endpoint dynamically probes target using container configuration', async (t) => {
    const fs = require('node:fs');
    const os = require('node:os');
    const path = require('node:path');
    const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'kb-health-test-'));
    t.after(() => fs.rmSync(tempDir, { recursive: true, force: true }));

    const containerId = 'test01020304';
    const cdir = path.join(tempDir, containerId);
    fs.mkdirSync(cdir, { recursive: true });
    fs.writeFileSync(path.join(cdir, 'state.json'), JSON.stringify({ id: containerId, status: 1 }));
    fs.writeFileSync(path.join(cdir, 'network.conf'), '10.0.100.15\n');
    fs.writeFileSync(path.join(cdir, 'ports.conf'), '3000:3000\n');

    const probeCalls = [];
    const mockHttpGet = (options, cb) => {
        probeCalls.push(options);
        const { EventEmitter } = require('node:events');
        const req = new EventEmitter();
        req.destroy = () => {};
        process.nextTick(() => {
            const res = new EventEmitter();
            res.statusCode = 200;
            cb(res);
            res.emit('data', JSON.stringify({ status: 'ok', uptime: 42 }));
            res.emit('end');
        });
        return req;
    };

    const server = createApp({
        containersDir: tempDir,
        httpGetImpl: mockHttpGet
    }).listen(0, '127.0.0.1');
    await new Promise(resolve => server.once('listening', resolve));
    t.after(() => new Promise(resolve => server.close(resolve)));

    const response = await request(server, 'GET', `/api/health/${containerId}`);
    assert.equal(response.status, 200);
    assert.equal(response.body.healthy, true);
    assert.equal(response.body.target, '10.0.100.15:3000');
    assert.equal(response.body.data.status, 'ok');
    assert.equal(probeCalls.length, 1);
    assert.equal(probeCalls[0].host, '10.0.100.15');
    assert.equal(probeCalls[0].port, 3000);
});

test('containers endpoint parses container list safely', async (t) => {
    const psOutput = 'CONTAINER ID   IMAGE               STATUS              NAMES\n' +
                     '8735d06fbbb0   demo-api:v1         Running             demo-1\n' +
                     '123456789abc   test:v2             Stopped (code 0)    test container\n';
    const server = createApp((file, args, options, callback) => {
        callback(null, psOutput, '');
    }).listen(0, '127.0.0.1');
    await new Promise(resolve => server.once('listening', resolve));
    t.after(() => new Promise(resolve => server.close(resolve)));

    const response = await request(server, 'GET', '/api/containers');
    assert.equal(response.status, 200);
    assert.equal(response.body.length, 2);
    assert.equal(response.body[0].id, '8735d06fbbb0');
    assert.equal(response.body[0].name, 'demo-1');
    assert.equal(response.body[1].id, '123456789abc');
    assert.equal(response.body[1].status, 'Stopped (code 0)');
    assert.equal(response.body[1].name, 'test container');
});
