import React from 'react';
import ReactDOM from 'react-dom/client';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { ReactQueryDevtools } from '@tanstack/react-query-devtools';
import { RouterProvider } from 'react-router';
import { router } from '@app/router';
import './styles/tokens.css';
import './styles/liquid-glass.css';
/* [284] 宿主窗口三键样式单独成文件：liquid-glass.css 有"零 display/flex/gap"
 * 全文件契约，窗口按钮必须 display:flex，混写会让契约自伤。 */
import './styles/window-chrome.css';

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
