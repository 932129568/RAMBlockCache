import React from 'react';
import ReactDOM from 'react-dom/client';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { ReactQueryDevtools } from '@tanstack/react-query-devtools';
import { RouterProvider } from 'react-router';
import { router } from '@app/router';
import './styles/tokens.css';

const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      // 控制通道是有状态的单句柄资源，不做自动重试与后台刷新，
      // 避免把瞬时失败放大成连续的 IOCTL 压力。
      retry: 0,
      refetchOnWindowFocus: false,
      staleTime: 0,
    },
  },
});

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <QueryClientProvider client={queryClient}>
      <RouterProvider router={router} />
      {import.meta.env.DEV && <ReactQueryDevtools initialIsOpen={false} />}
    </QueryClientProvider>
  </React.StrictMode>,
);
