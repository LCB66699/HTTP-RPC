import { useMemo, useState } from 'react'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { createRootRoute, createRoute, createRouter, RouterProvider } from '@tanstack/react-router'
import { AppShell } from './components/AppShell'
import { AuthScreen } from './features/auth/AuthScreen'
import { createApiClient, type User } from './lib/api'
import { SessionContext } from './lib/session'
import { MallPage, PointsPage, SearchPage } from './pages/CommercePages'
import { OperationsPage, OverviewPage, ProfilePage } from './pages/OperationsPages'
import { FilesPage, SheetEditorPage, SheetsPage, WorkspaceDetailPage, WorkspacesPage } from './pages/ResourcesPages'
import './styles.css'

function RouteFailure() {
  return <div className="page"><h1>Page unavailable</h1><p className="page-description">The requested page could not be rendered. Return to the overview and try again.</p></div>
}

const rootRoute = createRootRoute({ component: AppShell, errorComponent: RouteFailure, notFoundComponent: RouteFailure })
const overviewRoute = createRoute({ getParentRoute: () => rootRoute, path: '/', component: OverviewPage })
const sheetsRoute = createRoute({ getParentRoute: () => rootRoute, path: '/sheets', component: SheetsPage })
const sheetEditorRoute = createRoute({ getParentRoute: () => rootRoute, path: '/sheets/$sheetId', component: SheetEditorPage })
const filesRoute = createRoute({ getParentRoute: () => rootRoute, path: '/files', component: FilesPage })
const workspacesRoute = createRoute({ getParentRoute: () => rootRoute, path: '/workspaces', component: WorkspacesPage })
const workspaceDetailRoute = createRoute({ getParentRoute: () => rootRoute, path: '/workspaces/$workspaceId', component: WorkspaceDetailPage })
const pointsRoute = createRoute({ getParentRoute: () => rootRoute, path: '/points', component: PointsPage })
const mallRoute = createRoute({ getParentRoute: () => rootRoute, path: '/mall', component: MallPage })
const searchRoute = createRoute({ getParentRoute: () => rootRoute, path: '/search', component: SearchPage })
const operationsRoute = createRoute({ getParentRoute: () => rootRoute, path: '/operations', component: OperationsPage })
const profileRoute = createRoute({ getParentRoute: () => rootRoute, path: '/profile', component: ProfilePage })

const routeTree = rootRoute.addChildren([overviewRoute, sheetsRoute, sheetEditorRoute, filesRoute, workspacesRoute, workspaceDetailRoute, pointsRoute, mallRoute, searchRoute, operationsRoute, profileRoute])
const router = createRouter({ routeTree, defaultPreload: 'intent' })

declare module '@tanstack/react-router' {
  interface Register {
    router: typeof router
  }
}

export function App() {
  const queryClient = useQueryClient()
  const api = useMemo(() => createApiClient((input, init) => window.fetch(input, init)), [])
  const session = useQuery({ queryKey: ['session'], queryFn: api.getMe, retry: false, staleTime: Infinity })
  const [authenticatedUser, setAuthenticatedUser] = useState<User>()
  const user = authenticatedUser ?? session.data

  if (session.isPending && !user) return <main className="boot-screen" role="status">Restoring secure session...</main>
  if (!user) return <AuthScreen api={api} onAuthenticated={nextUser => { setAuthenticatedUser(nextUser); queryClient.setQueryData(['session'], nextUser) }} />

  const signOut = async () => {
    try {
      await api.logout()
    } finally {
      setAuthenticatedUser(undefined)
      queryClient.removeQueries()
      void router.navigate({ to: '/' })
    }
  }

  return <SessionContext.Provider value={{ api, user, signOut }}><RouterProvider router={router} /></SessionContext.Provider>
}
