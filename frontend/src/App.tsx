import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { z } from 'zod'
import { ApiError, createApiClient, type LoginInput, type User } from './lib/api'
import './styles.css'

const api = createApiClient(window.fetch.bind(window))

const loginSchema = z.object({
  username: z.string().trim().min(1, 'Username is required').max(64),
  password: z.string().min(1, 'Password is required').max(256)
})

function LoginScreen({ onAuthenticated }: { onAuthenticated: (user: User) => void }) {
  const [input, setInput] = useState<LoginInput>({ username: '', password: '' })
  const [error, setError] = useState<string>()
  const [submitting, setSubmitting] = useState(false)

  const submit = async (event: React.FormEvent<HTMLFormElement>) => {
    event.preventDefault()
    const parsed = loginSchema.safeParse(input)
    if (!parsed.success) {
      setError(parsed.error.issues[0]?.message)
      return
    }

    setSubmitting(true)
    setError(undefined)
    try {
      const response = await api.login(parsed.data)
      if (!response.success) {
        setError(response.error ?? 'Unable to sign in')
        return
      }
      onAuthenticated({
        user_id: response.user_id ?? 0,
        username: response.username ?? parsed.data.username,
        role: response.role
      })
    } catch (cause) {
      setError(cause instanceof ApiError ? cause.message : 'Unable to sign in')
    } finally {
      setSubmitting(false)
    }
  }

  return (
    <main className="auth-layout">
      <section className="auth-panel" aria-labelledby="app-title">
        <p className="eyebrow">SERVICE OPERATIONS</p>
        <h1 id="app-title">HTTP-RPC Console</h1>
        <p className="lede">Manage service discovery, requests, and operational workflows from one authenticated console.</p>
        <form onSubmit={submit} className="auth-form">
          <label>
            Username
            <input
              autoComplete="username"
              name="username"
              onChange={event => setInput(current => ({ ...current, username: event.target.value }))}
              value={input.username}
            />
          </label>
          <label>
            Password
            <input
              autoComplete="current-password"
              name="password"
              onChange={event => setInput(current => ({ ...current, password: event.target.value }))}
              type="password"
              value={input.password}
            />
          </label>
          {error && <p className="form-error" role="alert">{error}</p>}
          <button disabled={submitting} type="submit">{submitting ? 'Signing in...' : 'Sign in'}</button>
        </form>
        <a className="legacy-link" href="/legacy/">Open legacy console</a>
      </section>
    </main>
  )
}

function Dashboard({ user, onLogout }: { user: User; onLogout: () => void }) {
  const services = useQuery({
    queryKey: ['services'],
    queryFn: api.listServices,
    staleTime: 30_000
  })

  return (
    <main className="dashboard-layout">
      <header className="topbar">
        <div>
          <p className="eyebrow">HTTP-RPC</p>
          <h1>Service overview</h1>
        </div>
        <div className="account-actions">
          <span>{user.username}</span>
          <button className="secondary" onClick={onLogout} type="button">Sign out</button>
        </div>
      </header>
      <section className="dashboard-content" aria-labelledby="services-title">
        <div className="section-heading">
          <div>
            <h2 id="services-title">Registered services</h2>
            <p>Live data from the existing Go gateway.</p>
          </div>
          <a className="legacy-link" href="/legacy/">Open full legacy console</a>
        </div>
        {services.isPending && <p>Loading services...</p>}
        {services.isError && <p className="form-error" role="alert">Unable to load services. Open the legacy console to continue.</p>}
        {services.data && (
          <ul className="service-grid">
            {services.data.map(service => (
              <li key={service.name}>
                <h3>{service.name}</h3>
                <p>{service.description || 'No description provided.'}</p>
                <span>{service.methods?.length ?? 0} methods</span>
              </li>
            ))}
          </ul>
        )}
      </section>
    </main>
  )
}

export function App() {
  const [user, setUser] = useState<User>()

  if (!user) return <LoginScreen onAuthenticated={setUser} />

  return <Dashboard user={user} onLogout={() => {
    void api.logout().finally(() => setUser(undefined))
  }} />
}
