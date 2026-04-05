<?php
/**
 * XiaoZhi OTA version check endpoint
 * Deploy to: https://www.danscodellaro.com/esp32/xiaozhi/ota.php
 *
 * Device POSTs JSON with device info, we respond with firmware/config.
 */

header('Content-Type: application/json');

// --- Configuration ---
define('FIRMWARE_VERSION', '2.0.5.0');
define('FIRMWARE_URL',     'https://www.danscodellaro.com/esp32/xiaozhi/xiaozhi.bin');
define('WEBSOCKET_URL',    'wss://api.tenclass.net/xiaozhi/v1/');  // keep original for now
define('LOG_FILE',         __DIR__ . '/ota_requests.log');

// --- Log incoming request ---
$body    = file_get_contents('php://input');
$headers = getallheaders();
$log_entry = date('Y-m-d H:i:s') . ' '
    . ($headers['Device-Id']  ?? 'unknown') . ' '
    . ($headers['User-Agent'] ?? 'unknown') . ' '
    . trim($body) . PHP_EOL;
file_put_contents(LOG_FILE, $log_entry, FILE_APPEND);

// --- Build response ---
$response = [
    'firmware' => [
        'version' => FIRMWARE_VERSION,
        'url'     => FIRMWARE_URL,
    ],
    'websocket' => [
        'url' => WEBSOCKET_URL,
    ],
    'server_time' => [
        'timestamp'       => (int)(microtime(true) * 1000),
        'timezone_offset' => 600,  // AEST UTC+10
    ],
];

echo json_encode($response, JSON_PRETTY_PRINT);
