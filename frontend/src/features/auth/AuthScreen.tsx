import { useState } from 'react'
import { useForm } from 'react-hook-form'
import { z } from 'zod'
import type { ApiClient, LoginInput, User } from '../../lib/api'

const credentialsSchema = z.object({
  username: z.string().trim().min(3, 'Username must contain at least 3 characters').max(20, 'Username can contain at most 20 characters'),
  password: z.string().min(6, 'Password must contain at least 6 characters').max(256)
})

interface AuthScreenProps {
  api: ApiClient
  onAuthenticated: (user: User) => void
}

export function AuthScreen({ api, onAuthenticated }: AuthScreenProps) {
  const [mode, setMode] = useState<'login' | 'register'>('login')
  const [message, setMessage] = useState<string>()
  const form = useForm<LoginInput>({ defaultValues: { username: '', password: '' } })

  const submit = form.handleSubmit(async input => {
    const parsed = credentialsSchema.safeParse(input)
    if (!parsed.success) {
      form.setError('password', { message: parsed.error.issues[0]?.message })
      return
    }

    setMessage(undefined)
    try {
      const response = mode === 'login' ? await api.login(parsed.data) : await api.register(parsed.data)
      if (!response.success) {
        setMessage(response.error ?? 'Authentication failed')
        return
      }
      onAuthenticated({
        user_id: response.user_id ?? 0,
        username: response.username ?? parsed.data.username,
        role: response.role
      })
    } catch {
      setMessage('Unable to reach the authentication service')
    }
  })

  const isLogin = mode === 'login'
  return (
    <main className="auth-layout">
      <section className="auth-panel" aria-labelledby="app-title">
        <p className="eyebrow">SERVICE OPERATIONS</p>
        <h1 id="app-title">HTTP-RPC Console</h1>
        <p className="lede">A secure workspace for services, collaboration, files, and point-based fulfilment.</p>
        <form className="auth-form" onSubmit={submit} noValidate>
          <label>
            Username
            <input autoComplete="username" {...form.register('username')} />
          </label>
          <label>
            Password
            <input autoComplete={isLogin ? 'current-password' : 'new-password'} type="password" {...form.register('password')} />
          </label>
          {(form.formState.errors.password?.message || message) && (
            <p className="form-error" role="alert">{form.formState.errors.password?.message ?? message}</p>
          )}
          <button disabled={form.formState.isSubmitting} type="submit">
            {form.formState.isSubmitting ? 'Working...' : isLogin ? 'Sign in' : 'Create account'}
          </button>
        </form>
        <div className="auth-links">
          <button className="text-button" onClick={() => { form.clearErrors(); setMessage(undefined); setMode(isLogin ? 'register' : 'login') }} type="button">
            {isLogin ? 'Create an account' : 'Back to sign in'}
          </button>
          <a className="legacy-link" href="/legacy/">Open legacy console</a>
        </div>
      </section>
    </main>
  )
}
