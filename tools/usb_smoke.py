"""Check USB protocol v1 on a freshly booted, inactive ESP32 (requires pyserial).

This test never sends start. PHY wiring and an IO-Link device are not required.
The serial port must be available exclusively to this process.
"""

import argparse
import json
from pathlib import Path
import statistics
import time

import serial


parser = argparse.ArgumentParser()
parser.add_argument('--port', default='COM3')
parser.add_argument('--output', required=True)
args = parser.parse_args()
result = {'port': args.port, 'checks': [], 'started': time.strftime('%Y-%m-%dT%H:%M:%S%z')}
port = None


def connect():
    connection = serial.Serial(port=None, baudrate=115200, timeout=2, write_timeout=2)
    connection.dtr = True
    connection.rts = False
    connection.port = args.port
    connection.open()
    time.sleep(0.5)
    return connection


def exchange(wire, expected_id, error=None, fragments=False):
    started = time.perf_counter()
    if fragments:
        for byte in wire:
            port.write(bytes([byte]))
            time.sleep(0.002)
    else:
        port.write(wire)
    line = port.readline()
    assert line.endswith(b'\n'), f'Missing complete response: {line!r}'
    response = json.loads(line)
    assert response.get('id') == expected_id, response
    assert response.get('ok') is (error is None), response
    if error is not None:
        assert response.get('error') == error, response
    return response, (time.perf_counter() - started) * 1000


def request(request_id, operation, error=None, **fields):
    wire = (json.dumps({'id': request_id, 'op': operation, **fields}) + '\n').encode()
    return exchange(wire, request_id, error)


def record(name, detail):
    result['checks'].append({'name': name, 'result': 'pass', 'detail': detail})
    print(name + ': PASS', flush=True)


try:
    port = connect()
    response, _ = request(1, 'status')
    assert response['state'] == 'inactive' and response['protocol'] == 1, response
    record('boot_status', response)
    for request_id, operation, error, fields in [
        (2, 'info', 'not_ready', {}),
        (3, 'pd', 'data_invalid', {}),
        (4, 'isdu_read', 'isdu_rejected', {'index': 16, 'subindex': 0}),
        (5, 'isdu_write', 'isdu_rejected', {'index': 24, 'subindex': 0, 'data': '00'}),
        (6, 'bad_operation', 'unknown_operation', {}),
        (7, 'status', 'unknown_field', {'unexpected': True}),
        (8, 'isdu_read', 'invalid_index', {'index': 65536, 'subindex': 0}),
        (9, 'isdu_write', 'invalid_hex', {'index': 24, 'subindex': 0, 'data': '0'}),
    ]:
        response, _ = request(request_id, operation, error, **fields)
        record(f'request_{request_id}_{error}', response)
    response, _ = exchange(b'{"id":10,"op":"status"}\r\n', 10)
    record('crlf', response)
    response, _ = exchange(b'{"id":11,"op":"status"}\n', 11, fragments=True)
    record('fragmented_request', response)
    response, _ = exchange(b'{"id":12,"op":"status"}garbage\n', None, 'invalid_request')
    record('trailing_garbage', response)
    wire = b'{"id":13,"op":"status"}'
    response, _ = exchange(wire + b' ' * (768 - len(wire)) + b'\n', 13)
    record('maximum_line', response)
    response, _ = exchange(b'x' * 769 + b'\n', None, 'line_too_long')
    record('oversize_line', response)
    response, _ = request(14, 'status')
    assert response['state'] == 'inactive', response
    record('oversize_recovery', response)
    response, _ = request(15, 'stop')
    record('stop', response)
    durations = []
    for request_id in range(1000, 1500):
        response, duration = request(request_id, 'status')
        assert response['state'] == 'inactive' and not response['busy'], response
        durations.append(duration)
    record('status_500', {'count': len(durations), 'mean_ms': statistics.mean(durations),
                          'max_ms': max(durations), 'last_response': response})
    for request_id in range(2000, 2005):
        port.close()
        time.sleep(0.3)
        port = connect()
        response, _ = request(request_id, 'status')
        assert response['state'] == 'inactive', response
    record('reopen_5', response)
    result['result'] = 'pass'
except Exception as error:
    result['result'] = 'fail'
    result['failure'] = repr(error)
    print('FAIL:', repr(error), flush=True)
finally:
    if port is not None and port.is_open:
        port.close()
    Path(args.output).write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')

raise SystemExit(0 if result['result'] == 'pass' else 1)
