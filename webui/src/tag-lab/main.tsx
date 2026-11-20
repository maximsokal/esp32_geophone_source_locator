import { createRoot } from 'react-dom/client';
import { TagLabApp } from './TagLabApp';

const root = document.getElementById('root');
if (root === null) throw new Error('Tag Lab root element is missing.');
createRoot(root).render(<TagLabApp />);
