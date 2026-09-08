import { useEffect, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Search, ShoppingCart } from 'lucide-react'
import { EmptyState, ErrorState, LoadingState, PageHeader } from '../components/Page'
import { useSession } from '../lib/session'

function isoDate(value?: string) {
  return value ? new Date(value).toLocaleDateString() : '-'
}

export function PointsPage() {
  const { api } = useSession()
  const balance = useQuery({ queryKey: ['points', 'balance'], queryFn: api.getBalance })
  const transactions = useQuery({ queryKey: ['points', 'transactions'], queryFn: api.getTransactions })
  const leaderboard = useQuery({ queryKey: ['points', 'leaderboard'], queryFn: api.getLeaderboard })
  const failed = balance.isError || transactions.isError || leaderboard.isError
  return <div className="page"><PageHeader title="Points" description="Track the balance, transaction history, and leaderboard used by the fulfilment flow." />
    <section className="metric-grid"><article className="metric-card"><p>Current balance</p><strong>{balance.data?.balance ?? (balance.isPending ? '...' : '0')}</strong><span>available points</span></article></section>
    {failed && <ErrorState message="Some point data is temporarily unavailable." />}
    <div className="two-column"><section><h2>Recent transactions</h2>{transactions.isPending && <LoadingState />}{transactions.data?.transactions?.length ? <div className="data-list">{transactions.data.transactions.map(transaction => <div className="data-row" key={transaction.id}><span>{transaction.reason || transaction.type}</span><strong className={transaction.amount >= 0 ? 'positive' : 'negative'}>{transaction.amount >= 0 ? '+' : ''}{transaction.amount}</strong><small>{isoDate(transaction.created_at)}</small></div>)}</div> : !transactions.isPending && <EmptyState><p>No transactions yet.</p></EmptyState>}</section><section><h2>Leaderboard</h2>{leaderboard.isPending && <LoadingState />}{leaderboard.data?.entries?.length ? <ol className="leaderboard">{leaderboard.data.entries.map((entry, index) => <li key={entry.user_id}><span>#{index + 1} User {entry.user_id}</span><strong>{entry.total_earned}</strong></li>)}</ol> : !leaderboard.isPending && <EmptyState><p>No rankings yet.</p></EmptyState>}</section></div>
  </div>
}

function SeckillCountdown({ endAt, startAt }: { startAt?: number; endAt?: number }) {
  const [now, setNow] = useState(() => Date.now() / 1000)
  useEffect(() => { const timer = window.setInterval(() => setNow(Date.now() / 1000), 1_000); return () => window.clearInterval(timer) }, [])
  if (!startAt || !endAt) return null
  if (now < startAt) return <small>Starts in {Math.ceil(startAt - now)}s</small>
  if (now >= endAt) return <small>Ended</small>
  return <small>Ends in {Math.ceil(endAt - now)}s</small>
}

export function MallPage() {
  const { api } = useSession()
  const client = useQueryClient()
  const products = useQuery({ queryKey: ['mall', 'products'], queryFn: api.listProducts })
  const seckills = useQuery({ queryKey: ['mall', 'seckills'], queryFn: api.listSeckills })
  const orders = useQuery({ queryKey: ['mall', 'orders'], queryFn: api.listOrders })
  const refresh = () => Promise.all([client.invalidateQueries({ queryKey: ['mall'] }), client.invalidateQueries({ queryKey: ['points', 'balance'] })])
  const order = useMutation({ mutationFn: api.orderProduct, onSuccess: refresh })
  const seckillOrder = useMutation({ mutationFn: api.orderSeckill, onSuccess: refresh })
  return <div className="page"><PageHeader title="Mall" description="Redeem products and participate in active seckill inventory using your point balance." />
    {(order.isError || seckillOrder.isError) && <ErrorState message="The order could not be completed. The server has not confirmed a successful redemption." />}
    <section><h2>Products</h2>{products.isPending && <LoadingState />}{products.data?.products?.length ? <div className="card-grid">{products.data.products.map(product => <article className="product-card" key={product.id}><h3>{product.name}</h3><p>{product.description || 'No description'}</p><strong>{product.price} points</strong><button disabled={order.isPending} onClick={() => order.mutate(product.id)} type="button"><ShoppingCart aria-hidden="true" size={17} /> Redeem</button></article>)}</div> : !products.isPending && <EmptyState><p>No products are available.</p></EmptyState>}</section>
    <section className="section-gap"><h2>Seckill</h2>{seckills.isPending && <LoadingState />}{seckills.data?.seckills?.length ? <div className="card-grid">{seckills.data.seckills.map(item => { const active = (!item.start_at || item.start_at <= Date.now() / 1000) && (!item.end_at || item.end_at > Date.now() / 1000); return <article className="product-card" key={item.id}><h3>{item.product_name || `Product ${item.product_id ?? item.id}`}</h3><p>{item.seckill_stock ?? 0} remaining</p><strong>{item.seckill_price} points</strong><SeckillCountdown endAt={item.end_at} startAt={item.start_at} /><button disabled={!active || seckillOrder.isPending} onClick={() => seckillOrder.mutate(item.id)} type="button">{active ? 'Redeem now' : 'Unavailable'}</button></article> })}</div> : !seckills.isPending && <EmptyState><p>No seckill events are active.</p></EmptyState>}</section>
    <section className="section-gap"><h2>My orders</h2>{orders.isPending && <LoadingState />}{orders.data?.orders?.length ? <div className="data-list">{orders.data.orders.map(orderRow => <div className="data-row" key={orderRow.id}><span>{orderRow.product_name || `Order ${orderRow.id}`}</span><strong>{orderRow.amount ? `-${orderRow.amount}` : '-'}</strong><small>{orderRow.status || 'created'} · {isoDate(orderRow.created_at)}</small></div>)}</div> : !orders.isPending && <EmptyState><p>No orders yet.</p></EmptyState>}</section>
  </div>
}

export function SearchPage() {
  const { api } = useSession()
  const [input, setInput] = useState('')
  const [query, setQuery] = useState('')
  const search = useQuery({ queryKey: ['search', query], queryFn: () => api.search(query), enabled: query.length > 0 })
  return <div className="page"><PageHeader title="Search" description="Search the resources indexed by the existing gateway without injecting server-provided HTML." />
    <form className="search-form" onSubmit={event => { event.preventDefault(); setQuery(input.trim()) }}><input aria-label="Search resources" onChange={event => setInput(event.target.value)} placeholder="Search files and spreadsheets" value={input} /><button disabled={!input.trim()} type="submit"><Search aria-hidden="true" size={17} /> Search</button></form>
    {search.isPending && <LoadingState label="Searching..." />}{search.isError && <ErrorState message="Search is currently unavailable." />}{search.data && (search.data.results?.length ? <div className="resource-list">{search.data.results.map(result => <article className="resource-row" key={`${result.type}-${result.id}`}><div><h2>{result.title || result.name || `Resource ${result.id}`}</h2><p>{result.snippet || 'No preview available.'}</p><small>{result.type || 'resource'} · {isoDate(result.updated_at || result.created_at)}</small></div></article>)}</div> : <EmptyState><p>No resources matched “{query}”.</p></EmptyState>)}</div>
}
