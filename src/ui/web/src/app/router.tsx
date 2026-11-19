import { createHashRouter, Navigate } from 'react-router';
import { lazy, Suspense } from 'react';
import App from './App';

// [PERF-UI 2026-11-04] 路由级代码分割：六页静态 import 使单 chunk 超 500KB
// （构建警告），首屏要下载解析全部页面代码。lazy 化后首屏只载当前页，
// 其余按需——本地 HTTP 虽快，解析成本仍是首屏延迟的大头。
const StatusPage = lazy(() => import('@features/status/StatusPage'));
const DiagnosticsPage = lazy(() => import('@features/diagnostics/DiagnosticsPage'));
const BenefitPage = lazy(() => import('@features/benefit/BenefitPage'));
const VolumesPage = lazy(() => import('@features/volumes/VolumesPage'));
const EmergencyPage = lazy(() => import('@features/emergency/EmergencyPage'));
const SettingsPage = lazy(() => import('@features/settings/SettingsPage'));

// 采用 Hash 路由：本地桌面场景可能以 file:// 或自定义 scheme 加载，
// Browser 路由在非 HTTP 环境下会失配。六页全部实装（2026-10-31）。
function PageFallback() {
  return (
    <div style={{ padding: 32, color: 'var(--rbc-text-secondary, #666)' }}>
      加载页面…
    </div>
  );
}

export const router = createHashRouter([
  {
    path: '/',
    element: <App />,
    children: [
      { index: true, element: <Navigate to="/status" replace /> },
      { path: 'status', element: <Suspense fallback={<PageFallback />}><StatusPage /></Suspense> },
      { path: 'volumes', element: <Suspense fallback={<PageFallback />}><VolumesPage /></Suspense> },
      { path: 'benefit', element: <Suspense fallback={<PageFallback />}><BenefitPage /></Suspense> },
      { path: 'diagnostics', element: <Suspense fallback={<PageFallback />}><DiagnosticsPage /></Suspense> },
      { path: 'emergency', element: <Suspense fallback={<PageFallback />}><EmergencyPage /></Suspense> },
      { path: 'settings', element: <Suspense fallback={<PageFallback />}><SettingsPage /></Suspense> },
      { path: '*', element: <Navigate to="/status" replace /> },
    ],
  },
]);
