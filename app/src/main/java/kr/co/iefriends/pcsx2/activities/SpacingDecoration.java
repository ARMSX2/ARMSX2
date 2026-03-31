package kr.co.iefriends.pcsx2.activities;

import android.graphics.Rect;
import android.view.View;
import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

class SpacingDecoration extends RecyclerView.ItemDecoration {
    private int spacePx;

    SpacingDecoration(int spacePx) { this.spacePx = spacePx; }

    void updateSpacing(int spacePx) { this.spacePx = spacePx; }

    @Override
    public void getItemOffsets(@NonNull Rect outRect, @NonNull View view,
                               @NonNull RecyclerView parent, @NonNull RecyclerView.State state) {
        outRect.left = spacePx;
        outRect.right = spacePx;
        outRect.top = spacePx;
        outRect.bottom = spacePx;
    }
}
