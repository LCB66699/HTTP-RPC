import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { render, screen } from '@testing-library/react'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { App } from './App'

function renderApp() {
  return render(
    <QueryClientProvider client={new QueryClient({ defaultOptions: { queries: { retry: false } } })}>
      <App />
    </QueryClientProvider>
  )
}

describe('App', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
    window.history.replaceState({}, '', '/')
  })

  it('presents sign-in without rendering protected navigation when no session exists', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(null, { status: 401 })))

    renderApp()

    expect(await screen.findByRole('heading', { name: 'HTTP-RPC Console' })).toBeVisible()
    expect(screen.queryByRole('navigation', { name: 'Primary' })).not.toBeInTheDocument()
    expect(screen.getByRole('link', { name: 'Open legacy console' })).toHaveAttribute('href', '/legacy/')
  })

  it('restores a cookie-backed session and renders the requested protected route', async () => {
    window.history.replaceState({}, '', '/sheets')
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(
      new Response(JSON.stringify({ user_id: 7, username: 'alice', role: 'user' }), {
        status: 200,
        headers: { 'content-type': 'application/json' }
      })
    ))

    renderApp()

    expect(await screen.findByRole('heading', { name: 'Spreadsheets' })).toBeVisible()
    expect(screen.getByRole('navigation', { name: 'Primary' })).toBeVisible()
    expect(screen.getByText('alice')).toBeVisible()
  })
})
