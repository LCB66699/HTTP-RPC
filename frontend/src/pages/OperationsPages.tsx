import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { Activity, AlertTriangle, Server } from 'lucide-react'
import { EmptyState, ErrorState, LoadingState, PageHeader } from '../components/Page'
import { useSession } from '../lib/session'

export function OverviewPage() {
  const { api, user } = useSession()
  const health = useQuery({ queryKey: ['health'], queryFn: api.getHealth, refetchInterval: 30_000 })
  const services = useQuery({ queryKey: ['services'], queryFn: api.listServices })
  return <div className="page"><PageHeader title="Overview" description={`Signed in as ${user.username}. Gateway status refreshes every 30 seconds.`} />
    <section className="metric-grid"><article className="metric-card"><p>Gateway</p><strong>{health.data?.gateway ?? (health.isPending ? 'Checking' : 'Unknown')}</strong><span>live health endpoint</span></article><article className="metric-card"><p>Services</p><strong>{services.data?.length ?? (services.isPending ? '...' : '0')}</strong><span>registered by the gateway</span></article></section>
    {health.isError && <ErrorState message="Gateway health could not be confirmed." />}<section className="section-gap"><h2>Registered services</h2>{services.isPending && <LoadingState />}{services.data?.length ? <div className="resource-list">{services.data.map(service => <article className="resource-row" key={service.name}><div><h2>{service.name}</h2><p>{service.description || 'Service registered through the gateway.'}</p><small>{service.methods?.length ?? 0} known methods</small></div></article>)}</div> : !services.isPending && <EmptyState><Server aria-hidden="true" size={28} /><p>No services reported.</p></EmptyState>}</section>
  </div>
}

export function OperationsPage() {
  const { api } = useSession()
  const health = useQuery({ queryKey: ['health'], queryFn: api.getHealth, refetchInterval: 15_000 })
  const history = useQuery({ queryKey: ['history'], queryFn: api.getHistory })
  const states = health.data ? Object.entries(health.data).filter(([key]) => key !== 'gateway') : []
  return <div className="page"><PageHeader title="Operations" description="Inspect available service health and the request history exposed by the gateway." />
    {health.isPending && <LoadingState label="Checking service health..." />}{health.isError && <ErrorState message="Health endpoint is unavailable." />}{health.data && <div className="card-grid">{states.map(([name, value]) => { const state = typeof value === 'string' ? value : value.channel || value.breaker || 'Unknown'; return <article className="status-card" key={name}><Activity aria-hidden="true" size={18} /><h2>{name}</h2><strong>{state}</strong></article> })}</div>}
    <section className="section-gap"><h2>Request history</h2>{history.isPending && <LoadingState />}{history.isError && <ErrorState message="Request history is unavailable." />}{history.data?.entries?.length ? <ol className="history-list">{history.data.entries.map((entry, index) => <li key={`${entry}-${index}`}>{entry}</li>)}</ol> : !history.isPending && <EmptyState><AlertTriangle aria-hidden="true" size={28} /><p>No request history is available for this session.</p></EmptyState>}</section>
  </div>
}

export function ProfilePage() {
  const { api, user } = useSession()
  const [currentPassword, setCurrentPassword] = useState('')
  const [newPassword, setNewPassword] = useState('')
  const [message, setMessage] = useState<string>()
  const submit = async (event: React.FormEvent<HTMLFormElement>) => {
    event.preventDefault()
    if (newPassword.length < 6) { setMessage('New password must contain at least 6 characters.'); return }
    try { const result = await api.changePassword(currentPassword, newPassword); setMessage(result.success ? 'Password updated.' : result.error || 'Password update failed.'); if (result.success) { setCurrentPassword(''); setNewPassword('') } } catch { setMessage('Password update failed.') }
  }
  return <div className="page"><PageHeader title="Profile" description="Review the active account and update credentials through the authenticated API." /><section className="profile-panel"><h2>{user.username}</h2><p>User ID: {user.user_id}</p><p>Role: {user.role || 'user'}</p><form className="auth-form" onSubmit={submit}><label>Current password<input autoComplete="current-password" onChange={event => setCurrentPassword(event.target.value)} required type="password" value={currentPassword} /></label><label>New password<input autoComplete="new-password" minLength={6} onChange={event => setNewPassword(event.target.value)} required type="password" value={newPassword} /></label>{message && <p role="status">{message}</p>}<button type="submit">Update password</button></form></section></div>
}
