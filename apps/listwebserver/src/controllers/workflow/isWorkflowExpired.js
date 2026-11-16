// A started workflow with no update for 3 minutes is cancelled. lastUpdated is
// a Date.now() number, or undefined before the first update. moment(undefined)
// was "now", so such a workflow never expired; this keeps that rule.
const WORKFLOW_TIMEOUT_MS = 3 * 60 * 1000;

const isWorkflowExpired = (lastUpdated, now) =>
    lastUpdated !== undefined && now > Number(lastUpdated) + WORKFLOW_TIMEOUT_MS;

module.exports = { isWorkflowExpired, WORKFLOW_TIMEOUT_MS };
