// src/ui/web/src/components/LiquidGlassFilter.tsx
// 铁律: 落盘后同步登记至 build/sources.manifest.json (Gate16 check_build_manifest.py)
//
// [243 2026-11-15] iOS 26 filter pass, rewritten from
// deliverables/243-ios26-liquid-glass-and-fullstack-wiring-2026-11-15.md 3.3.
// Invariants kept for LiquidGlassFilter.test.tsx (5 assertions):
//   - #rbc-liquid-refract keeps 3 feColorMatrix + the ONLY feDisplacementMap
//     (scale = 0 under prefers-reduced-motion, otherwise the prop value);
//   - #rbc-liquid-gooey keeps its single feColorMatrix (4 matrices in total);
//   - #rbc-glass-specular exposes exactly one fePointLight with z = 96.
// Every <filter> declares colorInterpolationFilters="sRGB": the SVG default is
// linearRGB, which offsets the neutral-grey 128 in a displacement map and
// produces ghosting on glass edges.
import { useEffect, useState } from 'react';

export interface LiquidGlassFilterProps {
  /** 折射位移强度，默认常规态 9，减弱动态效果时自动降为 0 */
  refractScale?: number;
}

function usePrefersReducedMotion(): boolean {
  const [reduced, setReduced] = useState<boolean>(() => {
    if (typeof window === 'undefined' || !window.matchMedia) return false;
    return window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  });

  useEffect(() => {
    if (typeof window === 'undefined' || !window.matchMedia) return;
    const mq = window.matchMedia('(prefers-reduced-motion: reduce)');
    const onChange = () => setReduced(mq.matches);
    mq.addEventListener('change', onChange);
    return () => mq.removeEventListener('change', onChange);
  }, []);

  return reduced;
}

export function LiquidGlassFilter({ refractScale = 9 }: LiquidGlassFilterProps) {
  const reducedMotion = usePrefersReducedMotion();
  const effectiveScale = reducedMotion ? 0 : refractScale;

  return (
    <svg
      aria-hidden="true"
      focusable="false"
      width="0"
      height="0"
      style={{
        position: 'fixed',
        width: 0,
        height: 0,
        pointerEvents: 'none',
        overflow: 'hidden',
      }}
    >
      <defs>
        {/* 1. 既有契约滤镜：#rbc-liquid-refract (sRGB + 3 个 feColorMatrix + 唯一 feDisplacementMap) */}
        <filter
          id="rbc-liquid-refract"
          x="-12%"
          y="-12%"
          width="124%"
          height="124%"
          colorInterpolationFilters="sRGB"
        >
          <feTurbulence
            type="fractalNoise"
            baseFrequency="0.012 0.018"
            numOctaves={2}
            seed={7}
            result="refractNoise"
          />
          <feGaussianBlur in="refractNoise" stdDeviation="2.2" result="refractSmooth" />
          <feDisplacementMap
            in="SourceGraphic"
            in2="refractSmooth"
            scale={effectiveScale}
            xChannelSelector="R"
            yChannelSelector="G"
            result="refractedBase"
          />
          <feOffset in="refractedBase" dx={0.6} dy={0} result="redShift" />
          <feColorMatrix
            in="redShift"
            type="matrix"
            values="1 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 1 0"
            result="redCh"
          />
          <feOffset in="refractedBase" dx={-0.6} dy={0} result="blueShift" />
          <feColorMatrix
            in="blueShift"
            type="matrix"
            values="0 0 0 0 0  0 0 0 0 0  0 0 1 0 0  0 0 0 1 0"
            result="blueCh"
          />
          <feColorMatrix
            in="refractedBase"
            type="matrix"
            values="0 0 0 0 0  0 1 0 0 0  0 0 0 0 0  0 0 0 1 0"
            result="greenCh"
          />
          <feBlend in="redCh" in2="greenCh" mode="screen" result="rgBlend" />
          <feBlend in="rgBlend" in2="blueCh" mode="screen" />
        </filter>

        {/* 2. 既有契约滤镜：#rbc-liquid-gooey (粘滞液滴融合，含第 4 个 feColorMatrix) */}
        <filter id="rbc-liquid-gooey" colorInterpolationFilters="sRGB">
          <feGaussianBlur in="SourceGraphic" stdDeviation="6" result="gooBlur" />
          <feColorMatrix
            in="gooBlur"
            type="matrix"
            values="1 0 0 0 0  0 1 0 0 0  0 0 1 0 0  0 0 0 18 -7"
            result="gooMatrix"
          />
          <feComposite in="SourceGraphic" in2="gooMatrix" operator="atop" />
        </filter>

        {/* 3. iOS 26 控件级镜面点光源高光滤镜：#rbc-glass-specular (fePointLight z=96) */}
        <filter
          id="rbc-glass-specular"
          x="-10%"
          y="-10%"
          width="120%"
          height="120%"
          colorInterpolationFilters="sRGB"
        >
          <feGaussianBlur in="SourceAlpha" stdDeviation="2" result="specBlur" />
          <feSpecularLighting
            in="specBlur"
            surfaceScale={2}
            specularConstant={0.6}
            specularExponent={24}
            lightingColor="#ffffff"
            result="specLight"
          >
            <fePointLight x={80} y={-24} z={96} />
          </feSpecularLighting>
          <feComposite in="specLight" in2="SourceAlpha" operator="in" result="specClipped" />
          <feComposite
            in="SourceGraphic"
            in2="specClipped"
            operator="arithmetic"
            k1={0}
            k2={1}
            k3={0.22}
            k4={0}
          />
        </filter>
      </defs>
    </svg>
  );
}
