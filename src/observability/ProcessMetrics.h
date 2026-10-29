#pragma once

namespace urlshort {

class Metrics;

// Samples process-level CPU, resident memory and thread count where the
// platform exposes them. Called from the readiness loop so no extra thread is
// needed. Unsupported platforms are a no-op.
void sampleProcessMetrics(Metrics& metrics);

}  // namespace urlshort
