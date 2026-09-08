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

  it('sends a validated sheet-sharing request with same-origin credentials', async () => {
    const fetcher = vi.fn().mockResolvedValue(
      new Response(JSON.stringify({ success: true }), { status: 200, headers: { 'content-type': 'application/json' } })
    )
    const api = createApiClient(fetcher)

    await api.shareSheet(42, 'alice', 'edit')

    expect(fetcher).toHaveBeenCalledWith('/api/v1/sheets/42/share', expect.objectContaining({
      method: 'POST',
      credentials: 'same-origin',
      body: JSON.stringify({ username: 'alice', permission: 'edit' })
    }))
  })

  it('moves files only through the authenticated API client', async () => {
    const fetcher = vi.fn().mockResolvedValue(
      new Response(JSON.stringify({ success: true }), { status: 200, headers: { 'content-type': 'application/json' } })
    )
    const api = createApiClient(fetcher)

    await api.moveFile(7, 11)

    expect(fetcher).toHaveBeenCalledWith('/api/v1/files/7/move', expect.objectContaining({
      method: 'PUT',
      credentials: 'same-origin',
      body: JSON.stringify({ target_folder_id: 11 })
    }))
  })

  it('accepts canonical protobuf JSON string IDs', async () => {
    const fetcher = vi.fn().mockResolvedValue(
      new Response('{"success":true,"id":"90652009677533184"}', { status: 200, headers: { 'content-type': 'application/json' } })
    )
    const api = createApiClient(fetcher)

    const response = await api.createSheet({ name: 'Precision', description: '', headers_json: '[]', data_json: '[]' })

    expect(response.id).toBe('90652009677533184')
  })
})
