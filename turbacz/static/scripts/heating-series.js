// Shared four-second buckets for history and live readings, retaining one hour.
class HeatingSeries {
    constructor(stepSeconds = 4, windowSeconds = 3600) {
        this.stepSeconds = stepSeconds
        this.windowSeconds = windowSeconds
        this.points = new Map()
    }

    merge(samples, live = false, now = Date.now() / 1000) {
        const cutoff = Math.floor((now - this.windowSeconds) / this.stepSeconds) * this.stepSeconds
        for (const sample of samples) {
            const time = Number(sample.timestamp)
            if (!Number.isFinite(time) || time < cutoff || time > now + this.stepSeconds) continue
            const bucket = Math.floor(time / this.stepSeconds) * this.stepSeconds
            const previous = this.points.get(bucket)
            if (previous && ((previous.live && !live) || (previous.live === live && previous.time > time))) continue
            const point = { time, live }
            for (const key of ['cold', 'mixed', 'hot', 'target']) {
                const value = sample[key]
                point[key] = value !== null && value !== undefined && value !== '' && Number.isFinite(Number(value)) ? Number(value) : null
            }
            this.points.set(bucket, point)
        }
        for (const time of this.points.keys()) if (time < cutoff) this.points.delete(time)
    }

    datasets() {
        const times = [...this.points.keys()].sort((a, b) => a - b)
        if (!times.length) return [[], [], [], []]
        const result = [[], [], [], []]
        for (let time = times[0]; time <= times[times.length - 1]; time += this.stepSeconds) {
            const point = this.points.get(time)
            ;['cold', 'mixed', 'hot', 'target'].forEach((key, index) => {
                result[index].push({ x: time * 1000, y: point?.[key] ?? null })
            })
        }
        return result
    }
}

if (typeof module !== 'undefined') module.exports = { HeatingSeries }
