Sentry.init({
    dsn: window.SENTRY_DSN,
    integrations: [Sentry.browserTracingIntegration()],
    tracesSampleRate: 1.0,
    sendDefaultPii: true,
    tracePropagationTargets: ["localhost", /^https:\/\/turbacz\.dry\.pl\//],
});