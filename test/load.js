import http from 'k6/http';
import { check } from 'k6';

export const options = {
  scenarios: {
    ramp: {
      executor: 'ramping-arrival-rate',
      startRate: 100,
      timeUnit: '1s',
      preAllocatedVUs: 50,
      maxVUs: 500,
      stages: [
        { target: 500, duration: '10s' },
        { target: 1500, duration: '10s' },
        { target: 3000, duration: '10s' },
        { target: 5000, duration: '10s' },
        { target: 5000, duration: '20s' },
        { target: 0, duration: '5s' },
      ],
      gracefulStop: '5s',
    },
  },
  thresholds: {
    http_req_failed: ['rate<0.01'],
    http_req_duration: ['p(99)<50'],
  },
};

const URL = __ENV.URL || 'http://localhost:18080';
const PAYLOAD = JSON.stringify({
  id: 'tx-load',
  transaction: { amount: 100.0, installments: 1, requested_at: '2026-06-03T17:00:00Z' },
  customer: { avg_amount: 100.0, tx_count_24h: 1, known_merchants: ['MERC-001'] },
  merchant: { id: 'MERC-001', mcc: '5411', avg_amount: 100.0 },
  terminal: { is_online: true, card_present: true, km_from_home: 0.5 },
  last_transaction: { timestamp: '2026-06-03T16:00:00Z', km_from_current: 0.5 },
});

const params = { headers: { 'Content-Type': 'application/json' } };

export default function () {
  const res = http.post(`${URL}/fraud-score`, PAYLOAD, params);
  check(res, {
    'status 200': (r) => r.status === 200,
    'has approved': (r) => {
      try { return JSON.parse(r.body).approved !== undefined; } catch (_) { return false; }
    },
  });
}
