/**
 * DataTable — a small generic table.
 *
 * Three tables in this app (downloads, files, artifacts) share the same shell
 * but differ in columns, so the shell is generic and each caller supplies its
 * own column definitions. `layout: 'fixed'` column widths keep an elastic
 * column from forcing the table wider than its card.
 */

import type { ReactNode } from 'react';

export interface Column<T> {
  key: string;
  header: ReactNode;
  render: (row: T) => ReactNode;
  /** CSS width, e.g. '120px'. Omit for the elastic column. */
  width?: string;
  /** Right-align (numeric columns). */
  align?: 'left' | 'right';
  /** Extra class on both th and td. */
  className?: string;
}

export interface DataTableProps<T> {
  columns: ReadonlyArray<Column<T>>;
  rows: ReadonlyArray<T>;
  rowKey: (row: T) => string;
  onRowClick?: (row: T) => void;
  /** Rendered when `rows` is empty. */
  empty?: ReactNode;
  /** Extra class on the table element (used for per-table column widths). */
  className?: string;
}

export function DataTable<T>({
  columns,
  rows,
  rowKey,
  onRowClick,
  empty,
  className,
}: DataTableProps<T>) {
  if (rows.length === 0 && empty !== undefined) return <>{empty}</>;

  const cls = ['table', className].filter(Boolean).join(' ');
  return (
    <table className={cls}>
      <thead>
        <tr>
          {columns.map((c) => (
            <th
              key={c.key}
              scope="col"
              className={c.className}
              style={{
                width: c.width,
                textAlign: c.align === 'right' ? 'right' : undefined,
              }}
            >
              {c.header}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.map((row) => (
          <tr
            key={rowKey(row)}
            className={onRowClick ? 'is-clickable' : undefined}
            onClick={onRowClick ? () => onRowClick(row) : undefined}
          >
            {columns.map((c) => (
              <td
                key={c.key}
                className={c.className}
                style={{ textAlign: c.align === 'right' ? 'right' : undefined }}
              >
                {c.render(row)}
              </td>
            ))}
          </tr>
        ))}
      </tbody>
    </table>
  );
}
