package net.semtle.semtleuidemo;

import android.graphics.Rect;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewParent;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

/** Standard Android virtual view tree backed by the common HmjeUI snapshot.
 * All fields live on the main thread. Actions go to the real HMJE editor/router. */
final class HmjeAccessibilityProvider extends AccessibilityNodeProvider {
    interface Action { boolean perform(long identifier, int action); }
    private static final int WORDS = 18;
    private final View host;
    private final Action action;
    private final HashMap<Long, Integer> identities = new HashMap<>();
    private final HashMap<Integer, Node> nodes = new HashMap<>();
    private final ArrayList<Node> order = new ArrayList<>();
    private int nextIdentity = 1, accessibilityFocus = -1, hovered = -1, scale = 1;
    private static final class Node {
        int id, parent, role, state, actions;
        long semantic, parentSemantic, minimum, maximum, value;
        String label, text;
        final Rect rect = new Rect();
    }
    HmjeAccessibilityProvider(View host, Action action) { this.host = host; this.action = action; }
    private static boolean sameNode(Node a, Node b) {
        return a.semantic == b.semantic && a.parentSemantic == b.parentSemantic
            && a.role == b.role && a.state == b.state && a.actions == b.actions
            && a.rect.equals(b.rect) && a.minimum == b.minimum && a.maximum == b.maximum
            && a.value == b.value && a.label.equals(b.label) && a.text.equals(b.text);
    }
    void publish(long[] records, String[] labels, String[] values, int scale) {
        if (records.length % WORDS != 0 || records.length / WORDS != labels.length
            || labels.length != values.length) throw new IllegalArgumentException("AX snapshot length");
        ArrayList<Node> previous = new ArrayList<>(order);
        this.scale = Math.max(1, scale);
        HashMap<Long, Integer> next = new HashMap<>();
        nodes.clear(); order.clear();
        for (int i = 0; i < labels.length; ++i) {
            int b = i * WORDS;
            Node n = new Node(); n.semantic = records[b]; n.parentSemantic = records[b + 1];
            Integer existing = identities.get(n.semantic);
            n.id = existing == null ? nextIdentity++ : existing;
            next.put(n.semantic, n.id);
            n.role = (int)records[b + 2]; n.state = (int)records[b + 3];
            n.actions = (n.state & 1) != 0 ? 0 : (int)records[b + 4];
            n.label = labels[i]; n.text = (n.state & 32) != 0 ? "" : values[i];
            n.rect.set((int)records[b + 6] * this.scale, (int)records[b + 7] * this.scale,
                (int)(records[b + 6] + records[b + 8]) * this.scale,
                (int)(records[b + 7] + records[b + 9]) * this.scale);
            n.minimum = records[b + 14]; n.maximum = records[b + 15]; n.value = records[b + 16];
            nodes.put(n.id, n); order.add(n);
        }
        identities.clear(); identities.putAll(next);
        for (Node n : order) { Integer p = identities.get(n.parentSemantic); n.parent = p == null ? -1 : p; }
        if (!nodes.containsKey(accessibilityFocus)) accessibilityFocus = -1;
        if (!nodes.containsKey(hovered)) hovered = -1;
        // Repainting a caret or animation need not change the accessible tree.
        // Repeated identical subtree events prevent Android's idle detection
        // and needlessly wake accessibility services while an editor is focused.
        boolean changed = previous.size() != order.size();
        for (int i = 0; !changed && i < order.size(); ++i)
            changed = !sameNode(previous.get(i), order.get(i));
        if (changed) event(-1, AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
    }
    @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int id) {
        if (id == HOST_VIEW_ID) {
            AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(host);
            host.onInitializeAccessibilityNodeInfo(info);
            for (Node n : order) if (n.parent == -1) info.addChild(host, n.id);
            return info;
        }
        Node n = nodes.get(id); if (n == null) return null;
        AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain();
        info.setSource(host, id); info.setPackageName(host.getContext().getPackageName());
        if (n.parent == -1) info.setParent(host); else info.setParent(host, n.parent);
        String[] roles = {"", "android.widget.TextView", "android.widget.Button", "android.widget.EditText",
            "android.widget.Switch", "android.widget.CheckBox", "android.widget.SeekBar", "android.widget.ScrollView"};
        info.setClassName(roles[n.role]);
        info.setEnabled((n.state & 1) == 0); info.setSelected((n.state & 2) != 0);
        info.setVisibleToUser(host.isShown()); info.setFocusable(n.actions != 0);
        info.setFocused((n.state & 16) != 0); info.setPassword((n.state & 32) != 0);
        info.setAccessibilityFocused(accessibilityFocus == id);
        info.addAction(accessibilityFocus == id ? AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS : AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS);
        if (n.role == 3) {
            info.setEditable((n.state & 8) != 0); info.setMultiLine((n.state & 4) != 0);
            info.setText(n.text); info.setHintText(n.label); info.setContentDescription(n.label);
            if ((n.actions & 2) != 0) info.addAction(AccessibilityNodeInfo.ACTION_FOCUS);
        } else {
            info.setText(n.label);
            if (!n.text.isEmpty()) info.setStateDescription(n.text);
        }
        if (n.role == 4 || n.role == 5) { info.setCheckable(true); info.setChecked((n.state & 2) != 0); }
        if ((n.actions & 3) != 0) { info.setClickable(true); info.addAction(AccessibilityNodeInfo.ACTION_CLICK); }
        if ((n.actions & (4 | 32)) != 0) info.addAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD);
        if ((n.actions & (8 | 16)) != 0) info.addAction(AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD);
        info.setScrollable((n.actions & (16 | 32)) != 0);
        if (n.role == 6 && n.maximum >= n.minimum) info.setRangeInfo(AccessibilityNodeInfo.RangeInfo.obtain(
            AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT, n.minimum, n.maximum, n.value));
        Rect parentBounds = new Rect(n.rect); Node parent = nodes.get(n.parent);
        if (parent != null) parentBounds.offset(-parent.rect.left, -parent.rect.top);
        info.setBoundsInParent(parentBounds);
        int[] location = new int[2]; host.getLocationOnScreen(location);
        Rect screen = new Rect(n.rect); screen.offset(location[0], location[1]); info.setBoundsInScreen(screen);
        for (Node child : order) if (child.parent == id) info.addChild(host, child.id);
        return info;
    }
    @Override public AccessibilityNodeInfo findFocus(int focus) {
        if (focus == AccessibilityNodeInfo.FOCUS_ACCESSIBILITY) return accessibilityFocus == -1 ? null : createAccessibilityNodeInfo(accessibilityFocus);
        for (Node n : order) if ((n.state & 16) != 0) return createAccessibilityNodeInfo(n.id);
        return null;
    }
    @Override public List<AccessibilityNodeInfo> findAccessibilityNodeInfosByText(String text, int root) {
        ArrayList<AccessibilityNodeInfo> found = new ArrayList<>();
        for (Node n : order) if (n.label.contains(text) || ((n.state & 32) == 0 && n.text.contains(text))) {
            int p = n.id; while (p != -1 && p != root) { Node parent = nodes.get(p); p = parent == null ? -1 : parent.parent; }
            if (root == HOST_VIEW_ID || p == root) found.add(createAccessibilityNodeInfo(n.id));
        }
        return found;
    }
    @Override public boolean performAction(int id, int requested, Bundle arguments) {
        Node n = nodes.get(id); if (n == null) return false;
        if (requested == AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS) {
            if (accessibilityFocus == id) return false;
            int old = accessibilityFocus; accessibilityFocus = id;
            if (old != -1) event(old, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
            event(id, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED); return true;
        }
        if (requested == AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS) {
            if (accessibilityFocus != id) return false;
            accessibilityFocus = -1; event(id, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED); return true;
        }
        if ((n.state & 1) != 0) return false;
        int routed = 0;
        if (requested == AccessibilityNodeInfo.ACTION_CLICK) routed = (n.actions & 1) != 0 ? 1 : 2;
        if (requested == AccessibilityNodeInfo.ACTION_FOCUS) routed = 2;
        if (requested == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD) routed = (n.actions & 4) != 0 ? 4 : 32;
        if (requested == AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD) routed = (n.actions & 8) != 0 ? 8 : 16;
        return routed != 0 && (n.actions & routed) != 0 && action.perform(n.semantic, routed);
    }
    boolean hover(MotionEvent motion) {
        AccessibilityManager manager = (AccessibilityManager)host.getContext().getSystemService(android.content.Context.ACCESSIBILITY_SERVICE);
        if (manager == null || !manager.isEnabled() || !manager.isTouchExplorationEnabled()) return false;
        int next = -1;
        if (motion.getActionMasked() != MotionEvent.ACTION_HOVER_EXIT)
            for (Node n : order) if (n.rect.contains((int)motion.getX(), (int)motion.getY())) next = n.id;
        if (hovered != next) { if (hovered != -1) event(hovered, AccessibilityEvent.TYPE_VIEW_HOVER_EXIT); hovered = next;
            if (next != -1) event(next, AccessibilityEvent.TYPE_VIEW_HOVER_ENTER); }
        return next != -1;
    }
    private void event(int id, int type) {
        AccessibilityManager manager = (AccessibilityManager)host.getContext().getSystemService(android.content.Context.ACCESSIBILITY_SERVICE);
        if (manager == null || !manager.isEnabled()) return;
        AccessibilityEvent e = AccessibilityEvent.obtain(type); e.setPackageName(host.getContext().getPackageName());
        e.setSource(host, id);
        if (type == AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED) e.setContentChangeTypes(AccessibilityEvent.CONTENT_CHANGE_TYPE_SUBTREE);
        ViewParent parent = host.getParent(); if (parent != null) parent.requestSendAccessibilityEvent(host, e); else e.recycle();
    }
}
