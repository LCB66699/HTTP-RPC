import { describe, expect, it, vi } from 'vitest'
import { createApiClient } from './api'

describe('API client', () => {
  it('sends same-origin credentials on requests', async () => {
    const fetcher = vi.fn().mockResolvedValue(
      new Response(JSON.stringify({ user_id: 7, username: 'alice' }), { status: 200 })
    )
    const api = createApiClient(fetcher)

    await api.getMe()

    expect(fetcher).toHaveBeenCalledWith('/api/v1/me', expect.objectContaining({
      credentials: 'same-origin'
    }))
  })

  it('refreshes once after an unauthorized request and retries it', async () => {
    const fetcher = vi.fn()
      .mockResolvedValueOnce(new Response(null, { status: 401 }))
      .mockResolvedValueOnce(new Response(null, { status: 204 }))
      .mockResolvedValueOnce(
        new Response(JSON.stringify({ user_id: 7, username: 'alice' }), { status: 200 })
      )
    const api = createApiClient(fetcher)

    await api.getMe()

    expect(fetcher).toHaveBeenNthCalledWith(2, '/api/v1/refresh', expect.objectContaining({
      method: 'POST',
      credentials: 'same-origin'
    }))
    expect(fetcher).toHaveBeenCalledTimes(3)
  })
})
