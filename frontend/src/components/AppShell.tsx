import { Link, Outlet } from '@tanstack/react-router'
import { Archive, Boxes, ClipboardList, FileText, Gift, LayoutDashboard, LogOut, Search, Settings, Sheet, Users } from 'lucide-react'
import { useSession } from '../lib/session'

const navigation = [
  { to: '/', label: 'Overview', icon: LayoutDashboard },
  { to: '/sheets', label: 'Spreadsheets', icon: Sheet },
  { to: '/files', label: 'Files', icon: FileText },
  { to: '/workspaces', label: 'Workspaces', icon: Users },
  { to: '/points', label: 'Points', icon: Gift },
  { to: '/mall', label: 'Mall', icon: Archive },
  { to: '/search', label: 'Search', icon: Search },
  { to: '/operations', label: 'Operations', icon: Boxes },
  { to: '/profile', label: 'Profile', icon: Settings }
] as const

export function AppShell() {
  const { signOut, user } = useSession()
  return (
    <div className="app-shell">
      <aside className="sidebar">
        <Link className="brand" to="/">
          <ClipboardList aria-hidden="true" size={20} />
          <span>HTTP-RPC</span>
        </Link>
        <nav aria-label="Primary">
          {navigation.map(({ icon: Icon, label, to }) => (
            <Link activeProps={{ className: 'nav-link active' }} className="nav-link" key={to} to={to}>
              <Icon aria-hidden="true" size={18} />
              <span>{label}</span>
            </Link>
          ))}
        </nav>
        <div className="sidebar-footer">
          <span className="user-name">{user.username}</span>
          <button aria-label="Sign out" className="icon-button" onClick={() => void signOut()} title="Sign out" type="button">
            <LogOut aria-hidden="true" size={18} />
          </button>
        </div>
      </aside>
      <section className="app-content">
        <Outlet />
      </section>
    </div>
  )
}
