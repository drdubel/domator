(function loadSentry() {
    if (!window.SENTRY_DSN) {
        return
    }

    const sdk = document.createElement('script')
    sdk.src = 'https://browser.sentry-cdn.com/10.31.0/bundle.min.js'
    sdk.integrity = 'sha384-WVjomqmQqkLkhXoy0FyBbFsXlLswcYYTRk1GlSYKuJGIAh2OXZGZFlbL1VRFnRrz'
    sdk.crossOrigin = 'anonymous'
    sdk.referrerPolicy = 'strict-origin'
    sdk.async = true

    sdk.onload = function () {
        if (!window.Sentry) {
            console.warn('Sentry SDK loaded without exposing its browser API')
            return
        }

        window.Sentry.init({
            dsn: window.SENTRY_DSN,
            integrations: window.Sentry.browserTracingIntegration ? [window.Sentry.browserTracingIntegration()] : [],
            tracesSampleRate: window.SENTRY_TRACES_SAMPLE_RATE ?? 0.1,
            sendDefaultPii: false,
            tracePropagationTargets: ['localhost', window.location.origin],
        })
    }

    sdk.onerror = function () {
        console.warn('Sentry SDK could not be loaded; monitoring is disabled')
    }

    document.head.appendChild(sdk)
})()
