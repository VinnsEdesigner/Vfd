/**
 * Page skeletons.
 *
 * Each skeleton mirrors the layout of the page it stands in for, so the swap
 * from placeholder to content does not jump. Kept in one file because they are
 * small, closely related, and never imported individually by pages.
 */

import { Skeleton, SkeletonLines } from '../primitives/Skeleton';

export function OverviewSkeleton() {
  return (
    <>
      <section className="skeleton-stats">
        {Array.from({ length: 4 }, (_, i) => (
          <div className="skeleton-stat" key={i}>
            <Skeleton height={11} width="52%" />
            <Skeleton height={26} width="38%" />
            <Skeleton height={10} width="64%" />
          </div>
        ))}
      </section>

      <div className="grid-2">
        <div className="card">
          <div className="card__head">
            <Skeleton height={13} width="34%" />
          </div>
          <table className="table">
            <tbody>
              {Array.from({ length: 5 }, (_, r) => (
                <tr className="skeleton-row" key={r}>
                  <td><Skeleton height={12} width="70%" /></td>
                  <td><Skeleton height={12} width="86%" /></td>
                  <td><Skeleton height={12} width="58%" /></td>
                  <td><Skeleton height={12} width="62%" /></td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>

        <div className="stack">
          <div className="card">
            <div className="card__head">
              <Skeleton height={13} width="42%" />
            </div>
            <div className="card__body">
              <SkeletonLines lines={6} />
            </div>
          </div>
          <div className="card">
            <div className="card__head">
              <Skeleton height={13} width="52%" />
            </div>
            <div className="card__body">
              <Skeleton height={20} width="100%" />
            </div>
          </div>
        </div>
      </div>
    </>
  );
}

export function DownloadsSkeleton() {
  return (
    <div className="card">
      <div className="card__head">
        <Skeleton height={13} width="26%" />
      </div>
      <table className="table">
        <tbody>
          {Array.from({ length: 8 }, (_, r) => (
            <tr className="skeleton-row" key={r}>
              <td><Skeleton height={12} width="64%" /></td>
              <td><Skeleton height={12} width="88%" /></td>
              <td><Skeleton height={12} width="56%" /></td>
              <td><Skeleton height={12} width="72%" /></td>
              <td><Skeleton height={12} width="48%" /></td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

export function StorageSkeleton() {
  return (
    <>
      <div className="tabs" aria-hidden="true">
        <Skeleton height={14} width="88px" />
        <Skeleton height={14} width="120px" />
        <Skeleton height={14} width="132px" />
      </div>

      <div className="card">
        <div className="card__head">
          <Skeleton height={13} width="30%" />
        </div>
        <div className="card__body">
          <Skeleton height={11} width="18%" />
          <div className="dl-list" style={{ marginTop: 10 }}>
            {Array.from({ length: 2 }, (_, i) => (
              <div className="dl" key={i}>
                <div className="dl__top">
                  <Skeleton height={34} width={34} radius="6px" />
                  <div style={{ flex: 1 }}>
                    <Skeleton height={13} width="46%" />
                    <div style={{ height: 6 }} />
                    <Skeleton height={11} width="30%" />
                  </div>
                </div>
                <div className="dl__bar">
                  <Skeleton height={11} width="20%" />
                </div>
              </div>
            ))}
          </div>
        </div>
        <table className="table">
          <tbody>
            {Array.from({ length: 5 }, (_, r) => (
              <tr className="skeleton-row" key={r}>
                <td><Skeleton height={12} width="74%" /></td>
                <td><Skeleton height={12} width="56%" /></td>
                <td><Skeleton height={12} width="44%" /></td>
                <td><Skeleton height={12} width="80%" /></td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </>
  );
}

export function SettingsSkeleton() {
  return (
    <>
      <div className="tabs" aria-hidden="true">
        <Skeleton height={14} width="96px" />
        <Skeleton height={14} width="124px" />
        <Skeleton height={14} width="72px" />
      </div>
      <div className="grid-2 grid-2--even">
        {Array.from({ length: 2 }, (_, i) => (
          <div className="card" key={i}>
            <div className="card__head">
              <Skeleton height={13} width="38%" />
            </div>
            <div className="card__body">
              <SkeletonLines lines={4} />
            </div>
          </div>
        ))}
      </div>
    </>
  );
}

export function IngestSkeleton() {
  return (
    <div className="grid-2">
      <div className="card">
        <div className="card__head">
          <Skeleton height={13} width="24%" />
        </div>
        <div className="card__body">
          <Skeleton height={38} width="100%" radius="6px" />
          <div style={{ height: 18 }} />
          <SkeletonLines lines={3} />
        </div>
      </div>
      <div className="card">
        <div className="card__head">
          <Skeleton height={13} width="34%" />
        </div>
        <div className="card__body">
          <SkeletonLines lines={4} />
        </div>
      </div>
    </div>
  );
}
