const { isWorkflowExpired, WORKFLOW_TIMEOUT_MS } = require('./isWorkflowExpired');

const t = 1763290000000;

test('isWorkflowExpired: the timeout is 3 minutes', () => {
    expect(WORKFLOW_TIMEOUT_MS).toBe(3 * 60 * 1000);
});

// moment(undefined) is "now", so a workflow with no update never expired.
test('isWorkflowExpired: no update yet never expires', () => {
    expect(isWorkflowExpired(undefined, t)).toBe(false);
    expect(isWorkflowExpired(undefined, t + 10 * WORKFLOW_TIMEOUT_MS)).toBe(false);
});

// The old code cancelled only when lastUpdated + 3 min < now.
test('isWorkflowExpired: exactly 3 minutes does not expire', () => {
    expect(isWorkflowExpired(t, t + 180000)).toBe(false);
});

test('isWorkflowExpired: less than 3 minutes does not expire', () => {
    expect(isWorkflowExpired(t, t + 179999)).toBe(false);
});

test('isWorkflowExpired: more than 3 minutes expires', () => {
    expect(isWorkflowExpired(t, t + 180001)).toBe(true);
});

// moment() also takes a Date.
test('isWorkflowExpired: a Date works as a number does', () => {
    expect(isWorkflowExpired(new Date(t), t + 180001)).toBe(true);
    expect(isWorkflowExpired(new Date(t), t + 180000)).toBe(false);
});
