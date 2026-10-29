import http from 'k6/http';
import { check } from 'k6';
import { Counter } from 'k6/metrics';

http.setResponseCallback(http.expectedStatuses(201, 302));

const status201 = new Counter('status_201');
const status302 = new Counter('status_302');
const statusOther = new Counter('status_other');

const baseUrl = __ENV.BENCH_BASE_URL || 'http://127.0.0.1:8080';
const code = __ENV.BENCH_CODE;
const rate = Number(__ENV.BENCH_RATE || 1000);
const duration = __ENV.BENCH_DURATION || '20s';
const preAllocatedVUs = Number(__ENV.BENCH_PRE_VUS || 100);
const maxVUs = Number(__ENV.BENCH_MAX_VUS || 500);
const createPercent = Number(__ENV.BENCH_CREATE_PERCENT || 5);

export const options = {
  discardResponseBodies: true,
  scenarios: {
    mixed: {
      executor: 'constant-arrival-rate',
      rate,
      timeUnit: '1s',
      duration,
      preAllocatedVUs,
      maxVUs,
      gracefulStop: '10s',
    },
  },
};

export default function () {
  if (Math.random() * 100 < createPercent) {
    const body = JSON.stringify({
      url: `https://example.com/bench/${__VU}/${__ITER}/${Date.now()}`,
      expires_at: null,
    });
    const response = http.post(`${baseUrl}/api/v1/urls`, body, {
      headers: { 'Content-Type': 'application/json' },
      tags: { name: 'create' },
    });
    if (response.status === 201) {
      status201.add(1);
    } else {
      statusOther.add(1);
    }
    check(response, {
      'create status is 201': (r) => r.status === 201,
    });
    return;
  }

  const response = http.get(`${baseUrl}/${code}`, {
    redirects: 0,
    tags: { name: 'redirect' },
  });
  if (response.status === 302) {
    status302.add(1);
  } else {
    statusOther.add(1);
  }
  check(response, {
    'redirect status is 302': (r) => r.status === 302,
  });
}

