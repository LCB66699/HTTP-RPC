import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { render, screen } from '@testing-library/react'
import { describe, expect, it } from 'vitest'
import { App } from './App'

describe('App', () => {
  it('presents a sign-in screen when no authenticated session exists', async () => {
    render(
      <QueryClientProvider client={new QueryClient()}>
        <App />
      </QueryClientProvider>
    )

    expect(await screen.findByRole('heading', { name: 'HTTP-RPC Console' })).toBeVisible()
    expect(screen.getByRole('button', { name: 'Sign in' })).toBeVisible()
    expect(screen.getByRole('link', { name: 'Open legacy console' })).toHaveAttribute('href', '/legacy/')
  })
})
