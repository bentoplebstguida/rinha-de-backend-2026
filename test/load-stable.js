import http from 'k6/http';
import { check } from 'k6';

export const options = {
  scenarios: {
    stable: {
      executor: 'constant-arrival-rate',
      rate: 3000,
      timeUnit: '1s',
      duration: '30s',
      preAllocatedVUs: 100,
      maxVUs: 400,
    },
  },
  thresholds: {
    http_req_failed: ['rate<0.01'],
  },
};

const URL = __ENV.URL || 'http://localhost:19999';
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
