import type { ReactNode } from 'react'

export function PageHeader({ actions, description, title }: { title: string; description: string; actions?: ReactNode }) {
  return (
    <header className="page-header">
      <div>
        <p className="eyebrow">WORKSPACE</p>
        <h1>{title}</h1>
        <p className="page-description">{description}</p>
      </div>
      {actions && <div className="page-actions">{actions}</div>}
    </header>
  )
}

export function LoadingState({ label = 'Loading...' }: { label?: string }) {
  return <p className="status-message" role="status">{label}</p>
}

export function ErrorState({ message = 'The requested data could not be loaded.' }: { message?: string }) {
  return <p className="status-message error" role="alert">{message}</p>
}

export function EmptyState({ children }: { children: ReactNode }) {
  return <div className="empty-state">{children}</div>
}
